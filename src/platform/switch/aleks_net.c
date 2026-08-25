/*
 * switch_net.c — minimal HTTPS networking for the Switch port (issue #12 infra).
 *
 * Kept in its own TU (like switch_romfs.c / switch_applet.c) because <switch.h>
 * and <curl/curl.h> pull in u8/u32 etc. that clash with the GBA game headers.
 *
 * libcurl here is the devkitPro switch-curl portlib built against the libnx TLS
 * backend (the system `ssl` service), so HTTPS validates against the console's
 * own CA store automatically — no bundled cacert.pem. DNS/proxy come from the
 * socket driver. We only need socketInitializeDefault() at boot.
 *
 * This first cut exposes:
 *   - Port_Net_Init / Port_Net_Exit  — socket lifecycle (called from port_main)
 *   - Port_Net_HttpGet               — blocking GET, body into a caller buffer
 *   - Port_Net_SmokeTest             — one HTTPS GET, result logged to net.log,
 *                                      so the infra can be validated on hardware
 *                                      before any RetroAchievements code exists.
 *
 * RELEASE builds skip the SD log (same convention as the other switch_*.c).
 */
#include <switch.h>

#include <curl/curl.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* cwd is sdmc:/switch/tmc (port_main chdir'd there before the game starts). */
#ifndef TMC_RELEASE
static FILE* sLog = NULL;

static void nlog(const char* fmt, ...) {
    if (sLog == NULL) {
        sLog = fopen("net.log", "w");
        if (sLog == NULL) {
            return;
        }
        setvbuf(sLog, NULL, _IONBF, 0); /* unbuffered: survives a freeze */
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(sLog, fmt, ap);
    va_end(ap);
}
#else
static void nlog(const char* fmt, ...) { (void)fmt; }
#endif

static bool sNetReady = false;

/*
 * Show the Switch's native software keyboard and return what the user typed.
 * `header` is the prompt line; `password` hides the input (for the RA password).
 * Writes a NUL-terminated string into out[0..out_cap-1]. Returns 1 on OK, 0 if
 * the user cancelled or anything failed. Used by the RetroAchievements login
 * (port_retroachievements.c) — kept here because it needs <switch.h>.
 */
int Port_Swkbd_Get(const char* header, int password, char* out, size_t out_cap) {
    if (out_cap > 0) {
        out[0] = '\0';
    }
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) {
        return 0;
    }
    if (password) {
        swkbdConfigMakePresetPassword(&kbd);
    } else {
        swkbdConfigMakePresetDefault(&kbd);
    }
    if (header && header[0]) {
        /* Header text sits above the box and is easy to miss; GuideText shows
         * inside the (empty) box as a placeholder, so set both — that's what
         * makes it clear which field is being asked for. */
        swkbdConfigSetHeaderText(&kbd, header);
        swkbdConfigSetGuideText(&kbd, header);
    }
    rc = swkbdShow(&kbd, out, out_cap);
    swkbdClose(&kbd);
    if (R_FAILED(rc) || out[0] == '\0') {
        return 0; /* cancelled or empty */
    }
    return 1;
}

/* Bring up the socket driver. Safe to call once at boot. Library-applet launches
 * are already rejected earlier (issue #17), so the default socket config (which
 * reserves ~1-2 MiB of transfer memory) is fine — we only ever run in
 * Application mode with full memory here. */
void Port_Net_Init(void) {
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        nlog("[net] socketInitializeDefault failed: 0x%x\n", rc);
        sNetReady = false;
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    sNetReady = true;
    nlog("[net] socket + curl initialized\n");
}

void Port_Net_Exit(void) {
    if (!sNetReady) {
        return;
    }
    curl_global_cleanup();
    socketExit();
    sNetReady = false;
}

/* curl write callback: append received bytes into a fixed caller buffer,
 * truncating (never overflowing) if the response exceeds capacity. */
typedef struct {
    char*  buf;
    size_t cap;  /* total capacity incl. space for the NUL */
    size_t len;  /* bytes written so far (excl. NUL) */
} WriteCtx;

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t incoming = size * nmemb;
    WriteCtx* w = (WriteCtx*)userdata;
    size_t room = (w->cap > w->len + 1) ? (w->cap - w->len - 1) : 0;
    size_t take = incoming < room ? incoming : room;
    if (take > 0) {
        memcpy(w->buf + w->len, ptr, take);
        w->len += take;
        w->buf[w->len] = '\0';
    }
    /* Always claim the full amount so curl doesn't abort on truncation. */
    return incoming;
}

/* Growable buffer for responses of unknown size (RetroAchievements PatchData can
 * be tens of KB — a fixed buffer would silently truncate and break parsing). */
typedef struct {
    char*  buf;
    size_t cap;
    size_t len;
} DynBuf;

typedef struct {
    unsigned char* buf;
    size_t cap;
    size_t len;
    int overflow;
} BinaryBuf;

static size_t write_binary_bounded_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t incoming = size * nmemb;
    BinaryBuf* b = (BinaryBuf*)userdata;
    if (incoming > b->cap - b->len) {
        b->overflow = 1;
        return 0; /* Abort: badge files must never grow beyond the fixed cap. */
    }
    memcpy(b->buf + b->len, ptr, incoming);
    b->len += incoming;
    return incoming;
}

static size_t write_dyn_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t incoming = size * nmemb;
    DynBuf* d = (DynBuf*)userdata;
    if (d->len + incoming + 1 > d->cap) {
        size_t newcap = d->cap ? d->cap : 8192;
        while (newcap < d->len + incoming + 1) {
            newcap *= 2;
        }
        char* nb = (char*)realloc(d->buf, newcap);
        if (!nb) {
            return 0; /* out of memory — abort the transfer */
        }
        d->buf = nb;
        d->cap = newcap;
    }
    memcpy(d->buf + d->len, ptr, incoming);
    d->len += incoming;
    d->buf[d->len] = '\0';
    return incoming;
}

/*
 * Blocking HTTPS request (GET if post_data is NULL, else POST) into a
 * dynamically-grown buffer. On success *out_body points to a malloc'd,
 * NUL-terminated body the CALLER must free(); *out_len is its length. Returns
 * the HTTP status, or negative on transport failure (no allocation to free).
 * This is what the rc_client server-call uses, so large PatchData never
 * truncates.
 */
long Port_Net_HttpRequest(const char* url, const char* post_data, const char* content_type,
                          char** out_body, size_t* out_len) {
    *out_body = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (!sNetReady) {
        nlog("[net] HttpRequest called before init\n");
        return -1;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        return -2;
    }
    DynBuf d = { NULL, 0, 0 };
    struct curl_slist* headers = NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TMC-Switch/0.2 (libnx curl)");
    if (post_data) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
        if (content_type && content_type[0]) {
            char hdr[128];
            snprintf(hdr, sizeof hdr, "Content-Type: %s", content_type);
            headers = curl_slist_append(headers, hdr);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_dyn_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &d);

    CURLcode res = curl_easy_perform(curl);
    long status = -3;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        *out_body = d.buf; /* caller frees; NULL if the body was empty */
        if (out_len) {
            *out_len = d.len;
        }
    } else {
        nlog("[net] request failed: %s\n", curl_easy_strerror(res));
        free(d.buf);
    }
    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return status;
}

/*
 * Blocking HTTPS GET. Writes the (NUL-terminated, possibly truncated) response
 * body into out_body[0..out_cap-1]. Returns the HTTP status code (e.g. 200), or
 * a negative value on transport failure. out_len (optional) receives the body
 * length. This is the primitive the rc_client server-call callback will sit on.
 */
long Port_Net_HttpGet(const char* url, char* out_body, size_t out_cap, size_t* out_len) {
    if (out_cap > 0) {
        out_body[0] = '\0';
    }
    if (out_len) {
        *out_len = 0;
    }
    if (!sNetReady) {
        nlog("[net] HttpGet called before init\n");
        return -1;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        return -2;
    }
    WriteCtx ctx = { out_body, out_cap, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TMC-Switch/0.2 (libnx curl)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    long status = -3;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (out_len) {
            *out_len = ctx.len;
        }
    } else {
        nlog("[net] curl_easy_perform failed: %s\n", curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);
    return status;
}

/* Bounded binary GET for optional artwork. The caller owns *out_body on a
 * successful HTTP transfer and must free it. Unlike the RA JSON helper, this
 * deliberately rejects an over-size response instead of growing without
 * bound. It is safe to call from the port's existing background runner. */
long Port_Net_HttpGetBinaryBounded(const char* url, size_t max_bytes,
                                   unsigned char** out_body, size_t* out_len) {
    *out_body = NULL;
    if (out_len) *out_len = 0;
    if (!sNetReady || !url || !url[0] || max_bytes == 0) return -1;
    CURL* curl = curl_easy_init();
    if (!curl) return -2;
    unsigned char* body = (unsigned char*)malloc(max_bytes);
    if (!body) {
        curl_easy_cleanup(curl);
        return -4;
    }
    BinaryBuf b = { body, max_bytes, 0, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TMC-Switch/0.2 (libnx curl)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_binary_bounded_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    CURLcode res = curl_easy_perform(curl);
    long status = -3;
    if (res == CURLE_OK && !b.overflow) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 200 && status < 300 && b.len > 0) {
            *out_body = body;
            if (out_len) *out_len = b.len;
            body = NULL;
        }
    } else if (b.overflow) {
        status = -5;
    }
    free(body);
    curl_easy_cleanup(curl);
    return status;
}

/*
 * One-shot HTTPS smoke test, logged to sdmc:/switch/tmc/net.log. Proves the
 * whole path (socket init → DNS → TLS via system CA store → HTTP) end to end on
 * real hardware, independent of any RetroAchievements code. Hits the RA host so
 * we also confirm reachability of the service we actually care about.
 */
void Port_Net_SmokeTest(void) {
    static char body[1024];
    size_t len = 0;
    const char* url = "https://retroachievements.org/API/API_GetTopTenUsers.php";
    long status = Port_Net_HttpGet(url, body, sizeof body, &len);
    nlog("[net] smoke GET %s -> status=%ld, %zu bytes\n", url, status, len);
    if (len > 0) {
        size_t preview = len < 200 ? len : 200;
        nlog("[net] body[0..%zu]: %.*s\n", preview, (int)preview, body);
    }
}
