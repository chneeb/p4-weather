/*
 * Weather — current conditions and a 5-day forecast from Open-Meteo (open-meteo.com,
 * free for non-commercial use, data under CC BY 4.0). An example of the launcher's HTTP
 * interface: PAPP_MF_WIFI keeps Wi-Fi on while it runs, papp_http_get() fetches the forecast.
 *
 * Place and units: weather.txt in the app's data folder (/sd/data/chneeb.weather), written
 * with Munich on the first start.
 * place=City[, country or region] is looked up with Open-Meteo's geocoding search when it
 * changes; the result goes to name=, lat=, lon= (looked_up= remembers for which place).
 * Without place= (or with lat/lon edited by hand) the coordinates are used as they are.
 * The last answer is kept in cache.json beside it and shown when offline.
 * A = refresh, X / START or a tap = menu (Refresh, Units, Exit). Refreshes every 15 minutes.
 */
#include "papp/papp.h"
#include "papp_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "chneeb.weather", .name = "Weather", .version = PAPP_APP_VERSION,
              .main_stack = 32768, .flags = PAPP_MF_WIFI);

#define W 800
#define H 480
#define DAYS 5
#define DATA_DIR    "/sd/data/chneeb.weather"           /* the app's own folder (manifest id) */
#define CONFIG_PATH DATA_DIR "/weather.txt"
#define CACHE_PATH  DATA_DIR "/cache.json"

#define REFRESH_US  (15LL * 60 * 1000000)
#define BODY_MAX    16384

/* ── Settings ───────────────────────────────────────────────────────────── */

static struct {
    char place[64];              /* what the user typed: "Munich, Germany" */
    char looked_up[64];          /* the place name/lat/lon were found for */
    char name[48];
    char lat[16], lon[16];
    bool imperial;
} cfg = { "Munich, Germany", "Munich, Germany", "Munich", "48.1374", "11.5755", false };

static void config_save(void)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "# Weather app settings.\n"
               "# place: a city, optionally with its country or region (\"Munich, Germany\",\n"
               "#   \"Springfield, Illinois\"). The app looks it up on Open-Meteo and fills in name,\n"
               "#   lat and lon. For exact coordinates delete the place line and set lat / lon.\n"
               "# units: metric or imperial\n");
    if (cfg.place[0]) fprintf(f, "place=%s\n", cfg.place);
    fprintf(f, "units=%s\nname=%s\nlat=%s\nlon=%s\n", cfg.imperial ? "imperial" : "metric",
            cfg.name, cfg.lat, cfg.lon);
    if (cfg.place[0]) fprintf(f, "looked_up=%s\n", cfg.looked_up);
    fclose(f);
}

static bool place_changed(void)
{
    return cfg.place[0] && strcmp(cfg.place, cfg.looked_up) != 0;
}

static void config_load(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) { config_save(); return; }
    char line[128];
    cfg.place[0] = cfg.looked_up[0] = 0;         /* only what the file says */
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        while (*v == ' ') v++;
        if (!strcmp(k, "place")) snprintf(cfg.place, sizeof(cfg.place), "%s", v);
        else if (!strcmp(k, "looked_up")) snprintf(cfg.looked_up, sizeof(cfg.looked_up), "%s", v);
        else if (!strcmp(k, "name")) snprintf(cfg.name, sizeof(cfg.name), "%s", v);
        else if (!strcmp(k, "lat")) snprintf(cfg.lat, sizeof(cfg.lat), "%s", v);
        else if (!strcmp(k, "lon")) snprintf(cfg.lon, sizeof(cfg.lon), "%s", v);
        else if (!strcmp(k, "units")) cfg.imperial = !strcmp(v, "imperial");
    }
    fclose(f);
}

/* ── Forecast data and its JSON ─────────────────────────────────────────── */

static struct {
    bool ok;
    char time[20];                       /* current.time, "2026-11-07T18:15" (local) */
    float temp, feels, wind;
    int humidity, code, is_day;
    char day[DAYS][12];                  /* "2026-11-07" */
    int dcode[DAYS], rain[DAYS];
    float tmax[DAYS], tmin[DAYS];
} wx;

/* The object or array after "key": ({ ... } or [ ... ], strings skipped while matching) */
static bool json_object(const char *s, const char *key, const char **beg, const char **end)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = s;
    while ((p = strstr(p, pat)) && p[strlen(pat)] != '{' && p[strlen(pat)] != '[') p++;
    if (!p) return false;
    p += strlen(pat);
    int depth = 0;
    bool str = false;
    for (const char *q = p; *q; q++) {
        if (str) { if (*q == '\\' && q[1]) q++; else if (*q == '"') str = false; continue; }
        if (*q == '"') str = true;
        else if (*q == '{' || *q == '[') depth++;
        else if ((*q == '}' || *q == ']') && --depth == 0) { *beg = p; *end = q; return true; }
    }
    return false;
}

static const char *json_key(const char *beg, const char *end, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    size_t n = strlen(pat);
    for (const char *p = beg; p && p + n <= end; p++) {
        p = strstr(p, pat);
        if (!p || p + n > end) return NULL;
        return p + n;
    }
    return NULL;
}

static bool json_num(const char *beg, const char *end, const char *key, float *out)
{
    const char *v = json_key(beg, end, key);
    if (!v || *v == 'n') return false;            /* null */
    *out = strtof(v, NULL);
    return true;
}

static int json_nums(const char *beg, const char *end, const char *key, float *out, int max)
{
    const char *v = json_key(beg, end, key);
    if (!v || *v != '[') return 0;
    int n = 0;
    for (v++; n < max && v < end && *v != ']'; ) {
        char *next;
        out[n++] = *v == 'n' ? NAN : strtof(v, &next);
        if (*v == 'n') next = (char *)v + 4;
        v = next;
        while (v < end && (*v == ',' || *v == ' ')) v++;
    }
    return n;
}

static int json_strs(const char *beg, const char *end, const char *key, char out[][12], int max)
{
    const char *v = json_key(beg, end, key);
    if (!v || *v != '[') return 0;
    int n = 0;
    for (v++; n < max && v < end && *v == '"'; ) {
        const char *q = strchr(v + 1, '"');
        if (!q) break;
        snprintf(out[n++], 12, "%.*s", (int)(q - v - 1), v + 1);
        v = q + 1;
        while (v < end && (*v == ',' || *v == ' ')) v++;
    }
    return n;
}

static bool parse(const char *body)
{
    const char *b, *e;
    memset(&wx, 0, sizeof(wx));
    if (!json_object(body, "current", &b, &e)) return false;
    float v;
    const char *t = json_key(b, e, "time");
    if (t && *t == '"') snprintf(wx.time, sizeof(wx.time), "%.16s", t + 1);
    if (!json_num(b, e, "temperature_2m", &wx.temp)) return false;
    json_num(b, e, "apparent_temperature", &wx.feels);
    json_num(b, e, "wind_speed_10m", &wx.wind);
    if (json_num(b, e, "relative_humidity_2m", &v)) wx.humidity = (int)v;
    if (json_num(b, e, "weather_code", &v)) wx.code = (int)v;
    wx.is_day = json_num(b, e, "is_day", &v) ? (int)v : 1;

    if (!json_object(body, "daily", &b, &e)) return false;
    float a[DAYS];
    if (json_strs(b, e, "time", wx.day, DAYS) < DAYS) return false;
    json_nums(b, e, "weather_code", a, DAYS);
    for (int i = 0; i < DAYS; i++) wx.dcode[i] = (int)a[i];
    json_nums(b, e, "temperature_2m_max", wx.tmax, DAYS);
    json_nums(b, e, "temperature_2m_min", wx.tmin, DAYS);
    json_nums(b, e, "precipitation_probability_max", a, DAYS);
    for (int i = 0; i < DAYS; i++) wx.rain[i] = isnan(a[i]) ? -1 : (int)a[i];
    wx.ok = true;
    return true;
}

/* ── Fetch ──────────────────────────────────────────────────────────────── */

static char s_body[BODY_MAX];
static char s_status[128];                /* shown under the place name when not "" */

static void cache(bool save)
{
    FILE *f = fopen(CACHE_PATH, save ? "w" : "r");
    if (!f) return;
    if (save) fputs(s_body, f);
    else s_body[fread(s_body, 1, sizeof(s_body) - 1, f)] = 0;
    fclose(f);
}

static bool json_str(const char *beg, const char *end, const char *key, char *out, size_t n)
{
    const char *v = json_key(beg, end, key);
    if (!v || *v != '"') return false;
    const char *q = strchr(v + 1, '"');
    if (!q || q > end) return false;
    snprintf(out, n, "%.*s", (int)(q - v - 1), v + 1);
    return true;
}

static bool same_text(const char *a, const char *b)       /* ASCII case-insensitive */
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

static void url_encode(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 4 < n; p++) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                     *p == '-' || *p == '.' || *p == '_';
        if (plain) out[o++] = (char)*p;
        else o += snprintf(out + o, n - o, "%%%02X", *p);
    }
    out[o] = 0;
}

/* place "City[, country or region]" -> name, lat, lon via Open-Meteo's geocoding search.
 * Returns the HTTP status (200 with a match), 404 when nothing matches, or a PAPP_HTTP_ERR_* */
static int geocode(void)
{
    char city[64], filter[64] = "", enc[192], url[320];
    snprintf(city, sizeof(city), "%s", cfg.place);
    char *comma = strrchr(city, ',');
    if (comma) {
        *comma = 0;
        const char *f = comma + 1;
        while (*f == ' ') f++;
        snprintf(filter, sizeof(filter), "%s", f);
    }
    for (int n = (int)strlen(city); n > 0 && city[n - 1] == ' '; n--) city[n - 1] = 0;
    url_encode(enc, sizeof(enc), city);
    snprintf(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=20&language=en&format=json", enc);
    static char body[BODY_MAX];
    int st = papp_http_get(url, body, sizeof(body), NULL, 20000);
    if (st != 200) return st;

    const char *b, *e, *p;
    if (!json_object(body, "results", &b, &e)) return 404;       /* "results":[ ... ] */
    for (p = b + 1; p < e; ) {                                     /* each {...} in the array */
        const char *ob = strchr(p, '{');
        if (!ob || ob > e) break;
        const char *oe = ob;
        for (int depth = 0; *oe; oe++) {
            if (*oe == '{' || *oe == '[') depth++;
            else if ((*oe == '}' || *oe == ']') && --depth == 0) break;
        }
        char name[48] = "", cc[8] = "", country[48] = "", admin1[48] = "";
        float lat, lon;
        json_str(ob, oe, "name", name, sizeof(name));
        json_str(ob, oe, "country_code", cc, sizeof(cc));
        json_str(ob, oe, "country", country, sizeof(country));
        json_str(ob, oe, "admin1", admin1, sizeof(admin1));
        bool match = !filter[0] || same_text(filter, cc) || same_text(filter, country) || same_text(filter, admin1);
        if (match && json_num(ob, oe, "latitude", &lat) && json_num(ob, oe, "longitude", &lon)) {
            snprintf(cfg.name, sizeof(cfg.name), "%.47s", name[0] ? name : city);
            snprintf(cfg.lat, sizeof(cfg.lat), "%.4f", lat);
            snprintf(cfg.lon, sizeof(cfg.lon), "%.4f", lon);
            snprintf(cfg.looked_up, sizeof(cfg.looked_up), "%s", cfg.place);
            papp_log("weather: %s -> %s, %s %s (%s, %s)\n", cfg.place, cfg.name, cfg.lat, cfg.lon, admin1, country);
            config_save();
            return 200;
        }
        p = oe + 1;
    }
    return 404;
}

static void net_error(const char *what, int st)
{
    if (!papp_if_http()) snprintf(s_status, sizeof(s_status), "Needs a launcher with HTTP");
    else if (st == PAPP_HTTP_ERR_OFFLINE) snprintf(s_status, sizeof(s_status), "No Wi-Fi (Settings > Wi-Fi)");
    else if (st > 0) snprintf(s_status, sizeof(s_status), "Open-Meteo %s answered %d", what, st);
    else snprintf(s_status, sizeof(s_status), "No answer from Open-Meteo %s (%d)", what, st);
    papp_log("weather: %s failed: %d\n", what, st);
}

static void fetch(void)
{
    if (place_changed()) {
        int st = geocode();
        if (st == PAPP_HTTP_ERR_EXIT) return;
        if (st == 404) {
            wx.ok = false;                       /* the old place's weather would mislead */
            snprintf(cfg.name, sizeof(cfg.name), "%.47s", cfg.place);   /* on screen only */
            snprintf(s_status, sizeof(s_status), "Place not found (check %s)", CONFIG_PATH);
            return;
        }
        if (st != 200) {
            net_error("search", st);
            return;
        }
        wx.ok = false;
    }
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=%d%s",
             cfg.lat, cfg.lon, DAYS, cfg.imperial ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
    static char body[BODY_MAX];
    int st = papp_http_get(url, body, sizeof(body), NULL, 20000);
    if (st == 200 && strstr(body, "\"current\":{")) {
        memcpy(s_body, body, sizeof(s_body));
        parse(s_body);
        cache(true);
        s_status[0] = 0;
        return;
    }
    if (st == PAPP_HTTP_ERR_EXIT) return;
    net_error("forecast", st);
}

/* ── Drawing: anti-aliased shapes ───────────────────────────────────────── */

static uint16_t *s_img;                  /* the picture, shown with present_ext (64-byte aligned) */
static papp_canvas_t C;

static inline void plot(int x, int y, uint16_t col, float a)
{
    if (a <= 0 || x < 0 || y < 0 || x >= W || y >= H) return;
    uint16_t *p = &s_img[y * W + x];
    *p = a >= 1 ? col : papp_blend565(*p, col, (int)(a * 256));
}

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void disc(float cx, float cy, float r, uint16_t col)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            plot(x, y, col, clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
}

/* A line with round ends, w wide */
static void stroke(float x0, float y0, float x1, float y1, float w, uint16_t col)
{
    float r = w / 2, vx = x1 - x0, vy = y1 - y0, ll = vx * vx + vy * vy;
    int xa = (int)(fminf(x0, x1) - r - 1), xb = (int)(fmaxf(x0, x1) + r + 1);
    int ya = (int)(fminf(y0, y1) - r - 1), yb = (int)(fmaxf(y0, y1) + r + 1);
    for (int y = ya; y <= yb; y++)
        for (int x = xa; x <= xb; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = ll > 0 ? clamp01((px * vx + py * vy) / ll) : 0;
            float dx = px - t * vx, dy = py - t * vy;
            plot(x, y, col, clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
}

/* Disc (cx, cy, r) minus disc (cx2, cy2, r2) */
static void crescent(float cx, float cy, float r, float cx2, float cy2, float r2, uint16_t col)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, ex = x + 0.5f - cx2, ey = y + 0.5f - cy2;
            plot(x, y, col, clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f) *
                            clamp01(sqrtf(ex * ex + ey * ey) - r2 + 0.5f));
        }
}

static void ring(float cx, float cy, float r, float w, uint16_t col)
{
    for (int y = (int)(cy - r - w); y <= (int)(cy + r + w); y++)
        for (int x = (int)(cx - r - w); x <= (int)(cx + r + w); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            plot(x, y, col, clamp01(w / 2 - fabsf(d - r) + 0.5f));
        }
}

static void fill_gradient(int y0, int y1, int r0, int g0, int b0, int r1, int g1, int b1)
{
    for (int y = y0; y < y1; y++) {
        int t = (y - y0) * 256 / (y1 - y0);
        uint16_t c = PAPP_RGB565(r0 + (r1 - r0) * t / 256, g0 + (g1 - g0) * t / 256, b0 + (b1 - b0) * t / 256);
        for (int x = 0; x < W; x++) s_img[y * W + x] = c;
    }
}

/* ── Icons: drawn in a size x size box at (x, y) ────────────────────────── */

enum { IC_SUN, IC_PARTLY, IC_CLOUD, IC_FOG, IC_DRIZZLE, IC_RAIN, IC_SHOWERS, IC_SNOW, IC_THUNDER };

static int icon_of(int code)
{
    switch (code) {
    case 0: case 1: return IC_SUN;
    case 2: return IC_PARTLY;
    case 3: return IC_CLOUD;
    case 45: case 48: return IC_FOG;
    case 51: case 53: case 55: case 56: case 57: return IC_DRIZZLE;
    case 61: case 63: case 65: case 66: case 67: return IC_RAIN;
    case 80: case 81: case 82: return IC_SHOWERS;
    case 71: case 73: case 75: case 77: case 85: case 86: return IC_SNOW;
    case 95: case 96: case 99: return IC_THUNDER;
    default: return IC_CLOUD;
    }
}

static const char *describe(int code)
{
    switch (code) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: return "Fog";
    case 48: return "Rime fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Rain showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    default: return "";
    }
}

#define SUN_C    PAPP_RGB565(255, 200, 40)
#define MOON_C   PAPP_RGB565(240, 232, 190)
#define CLOUD_C  PAPP_RGB565(236, 241, 247)
#define DARK_C   PAPP_RGB565(150, 162, 180)
#define DROP_C   PAPP_RGB565(90, 175, 255)
#define SNOW_C   PAPP_RGB565(255, 255, 255)
#define BOLT_C   PAPP_RGB565(255, 214, 60)

static void sun(float x, float y, float s)
{
    float cx = x + s / 2, cy = y + s / 2;
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f, c = cosf(a), d = sinf(a);
        stroke(cx + c * s * 0.31f, cy + d * s * 0.31f, cx + c * s * 0.43f, cy + d * s * 0.43f, s * 0.06f, SUN_C);
    }
    disc(cx, cy, s * 0.22f, SUN_C);
}

static void moon(float x, float y, float s)
{
    crescent(x + s * 0.5f, y + s * 0.5f, s * 0.3f, x + s * 0.64f, y + s * 0.38f, s * 0.25f, MOON_C);
}

static void cloud(float x, float y, float s, uint16_t col)
{
    disc(x + s * 0.32f, y + s * 0.58f, s * 0.16f, col);
    disc(x + s * 0.52f, y + s * 0.46f, s * 0.22f, col);
    disc(x + s * 0.71f, y + s * 0.60f, s * 0.14f, col);
    stroke(x + s * 0.32f, y + s * 0.66f, x + s * 0.71f, y + s * 0.66f, s * 0.16f, col);
}

static void drops(float x, float y, float s, int n)
{
    for (int i = 0; i < n; i++) {
        float dx = x + s * (0.36f + 0.15f * i);
        stroke(dx, y + s * 0.80f, dx - s * 0.04f, y + s * 0.92f, s * 0.05f, DROP_C);
    }
}

static void icon(int kind, bool day, float x, float y, float s)
{
    switch (kind) {
    case IC_SUN:
        if (day) sun(x, y, s); else moon(x, y, s);
        break;
    case IC_PARTLY:
    case IC_SHOWERS:
        if (day) sun(x - s * 0.12f, y - s * 0.14f, s * 0.75f);
        else moon(x - s * 0.1f, y - s * 0.14f, s * 0.75f);
        cloud(x + s * 0.06f, y + s * (kind == IC_SHOWERS ? 0.0f : 0.06f), s, kind == IC_SHOWERS ? DARK_C : CLOUD_C);
        if (kind == IC_SHOWERS) drops(x + s * 0.06f, y, s, 3);
        break;
    case IC_CLOUD:
        cloud(x, y - s * 0.04f, s, CLOUD_C);
        break;
    case IC_FOG:
        for (int i = 0; i < 4; i++)
            stroke(x + s * (0.2f + 0.05f * (i & 1)), y + s * (0.32f + 0.14f * i),
                   x + s * (0.8f - 0.05f * (i & 1)), y + s * (0.32f + 0.14f * i), s * 0.07f, CLOUD_C);
        break;
    case IC_DRIZZLE:
    case IC_RAIN:
        cloud(x, y - s * 0.1f, s, DARK_C);
        drops(x, y, s, kind == IC_RAIN ? 3 : 2);
        break;
    case IC_SNOW:
        cloud(x, y - s * 0.1f, s, DARK_C);
        for (int i = 0; i < 3; i++) {
            float fx = x + s * (0.34f + 0.16f * i), fy = y + s * (0.84f + (i & 1) * 0.04f), r = s * 0.05f;
            for (int k = 0; k < 3; k++) {
                float a = k * 1.0472f;
                stroke(fx - cosf(a) * r, fy - sinf(a) * r, fx + cosf(a) * r, fy + sinf(a) * r, s * 0.025f, SNOW_C);
            }
        }
        break;
    case IC_THUNDER:
        cloud(x, y - s * 0.12f, s, DARK_C);
        stroke(x + s * 0.56f, y + s * 0.66f, x + s * 0.46f, y + s * 0.80f, s * 0.06f, BOLT_C);
        stroke(x + s * 0.46f, y + s * 0.80f, x + s * 0.57f, y + s * 0.80f, s * 0.06f, BOLT_C);
        stroke(x + s * 0.57f, y + s * 0.80f, x + s * 0.46f, y + s * 0.95f, s * 0.06f, BOLT_C);
        break;
    }
}

/* ── Screen ─────────────────────────────────────────────────────────────── */

#define TEXT_C  PAPP_RGB565(255, 255, 255)
#define DIM_C   PAPP_RGB565(200, 215, 235)
#define WARN_C  PAPP_RGB565(255, 200, 120)

static int iround(float v) { return (int)floorf(v + 0.5f); }

static const char *weekday(const char *date)          /* "2026-11-07" */
{
    static const char *const names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = atoi(date), m = atoi(date + 5), d = atoi(date + 8);
    if (m < 1 || m > 12) return "";
    if (m < 3) y--;
    return names[(y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7];
}

static void text_right(int xr, int y, const char *s, uint16_t col, int scale, bool smooth)
{
    int x = xr - papp_text_width(s, scale);
    if (smooth) papp_text_smooth(&C, x, y, s, col, scale);
    else papp_text(&C, x, y, s, col, scale);
}

static void draw(bool busy)
{
    bool day = !wx.ok || wx.is_day;
    if (day) fill_gradient(0, H, 40, 96, 170, 110, 165, 220);
    else fill_gradient(0, H, 10, 16, 42, 36, 48, 92);
    char s[200];

    papp_text_smooth(&C, 24, 16, cfg.name, TEXT_C, 3);
    papp_datetime_t now;
    if (papp_local_time(&now)) {
        snprintf(s, sizeof(s), "%02d:%02d", now.hour, now.minute);
        text_right(W - 24, 16, s, TEXT_C, 3, true);
    }
    if (busy) snprintf(s, sizeof(s), "Updating...");
    else if (s_status[0]) snprintf(s, sizeof(s), "%s", s_status);
    else if (wx.ok) snprintf(s, sizeof(s), "as of %s", wx.time[0] ? wx.time + 11 : "?");
    else s[0] = 0;
    papp_text(&C, 26, 66, s, s_status[0] && !busy ? WARN_C : DIM_C, 1);
    if (s_status[0] && wx.ok && !busy) {
        snprintf(s, sizeof(s), "showing %s %s", wx.time[0] ? wx.time + 11 : "", "(saved)");
        papp_text(&C, 26 + papp_text_width(s_status, 1) + 16, 66, s, DIM_C, 1);
    }

    if (!wx.ok) {
        papp_text_centered(&C, W / 2, 200, busy ? "Getting the weather..." : "No weather data yet", TEXT_C, 3);
        papp_text_centered(&C, W / 2, 260, "A: refresh   X / START / tap: menu", DIM_C, 2);
        text_right(W - 12, H - 20, "Weather data: Open-Meteo.com", DIM_C, 1, false);
        return;
    }

    const char *unit = cfg.imperial ? "F" : "C";
    icon(icon_of(wx.code), wx.is_day, 30, 92, 180);
    snprintf(s, sizeof(s), "%d", iround(wx.temp));
    int x = papp_text_smooth(&C, 230, 88, s, TEXT_C, 9);
    ring(x + 20, 120, 13, 7, TEXT_C);                  /* the font's degree sign is too thin this big */
    papp_text_smooth(&C, x + 42, 104, unit, TEXT_C, 4);
    papp_text_smooth(&C, 236, 236, describe(wx.code), TEXT_C, 3);

    int dx = 560, dy = 104;
    snprintf(s, sizeof(s), "Feels %d\xc2\xb0", iround(wx.feels));
    papp_text_smooth(&C, dx, dy, s, TEXT_C, 2);
    snprintf(s, sizeof(s), "Humidity %d%%", wx.humidity);
    papp_text_smooth(&C, dx, dy + 44, s, TEXT_C, 2);
    snprintf(s, sizeof(s), "Wind %d %s", iround(wx.wind), cfg.imperial ? "mph" : "km/h");
    papp_text_smooth(&C, dx, dy + 88, s, TEXT_C, 2);

    papp_canvas_fill(&C, 24, 290, W - 48, 2, DIM_C);
    int cw = W / DAYS;
    for (int i = 0; i < DAYS; i++) {
        int cx = i * cw + cw / 2;
        papp_text_centered(&C, cx, 300, i == 0 ? "Today" : weekday(wx.day[i]), TEXT_C, 2);
        icon(icon_of(wx.dcode[i]), true, cx - 34, 334, 68);
        snprintf(s, sizeof(s), "%d\xc2\xb0 %d\xc2\xb0", iround(wx.tmax[i]), iround(wx.tmin[i]));
        papp_text_centered(&C, cx, 404, s, TEXT_C, 2);
        if (wx.rain[i] >= 0) {
            snprintf(s, sizeof(s), "rain %d%%", wx.rain[i]);
            papp_text_centered(&C, cx, 438, s, DIM_C, 1);
        }
    }
    text_right(W - 12, H - 20, "Weather data: Open-Meteo.com", DIM_C, 1, false);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

/* `-- icons`: every icon by day (top) and night (bottom), for checking the drawings */
static void draw_icons(void)
{
    fill_gradient(0, H / 2, 40, 96, 170, 110, 165, 220);
    fill_gradient(H / 2, H, 10, 16, 42, 36, 48, 92);
    for (int k = 0; k <= IC_THUNDER; k++) {
        icon(k, true, 8 + k * 88, 40, 80);
        icon(k, false, 8 + k * 88, 280, 80);
    }
    papp_text(&C, 8, 8, "sun partly cloud fog drizzle rain showers snow thunder", TEXT_C, 1);
}

int main(int argc, char **argv)
{
    if (papp_fs()) {                            /* fails harmlessly when they exist */
        papp_fs()->mkdir("/sd/data");
        papp_fs()->mkdir(DATA_DIR);
    }
    config_load();
    papp_surface_cfg_t sc = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                              .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    papp_surface_t *surf = papp_disp_surface_create(&sc);
    s_img = papp_alloc(W * H * 2, PAPP_MEM_ANY, 64);
    if (!surf || !s_img) return 1;
    C = (papp_canvas_t){ s_img, W, W, H };

    if (argc > 1 && !strcmp(argv[1], "icons")) {
        draw_icons();
        while (!papp_exit_requested()) {
            papp_disp_present_ext(surf, s_img);
            papp_delay_ms(100);
        }
        papp_disp_surface_destroy(surf);
        papp_free(s_img);
        return 0;
    }

    if (!place_changed()) {                     /* the saved answer is for another place otherwise */
        cache(false);
        if (s_body[0]) parse(s_body);
    }

    int64_t next_fetch = 0;
    int last_minute = -1;
    bool dirty = true, was_touch = false;
    uint32_t prev = 0xFFFFFFFFu;
    while (!papp_exit_requested()) {
        int64_t now = papp_time_us();
        if (now >= next_fetch) {
            draw(true);
            papp_disp_present_ext(surf, s_img);
            fetch();
            next_fetch = papp_time_us() + REFRESH_US;
            dirty = true;
        }

        papp_gamepad_t pad = { 0 };
        papp_input_gamepad(0, &pad);
        uint32_t press = pad.buttons & ~prev;
        prev = pad.buttons;
        papp_touch_point_t tp;
        bool touch = papp_disp_touch(surf, &tp, 1) > 0;
        bool tap = was_touch && !touch;
        was_touch = touch;

        if (press & PAPP_BTN_A) next_fetch = 0;
        if (tap || (press & (PAPP_BTN_X | PAPP_BTN_START))) {
            const char *items[] = { "Refresh", cfg.imperial ? "Units: imperial" : "Units: metric", "Exit" };
            int r = papp_ui_menu("Weather", items, 3, 0);
            prev = 0xFFFFFFFFu;                /* ignore what closed the menu until released */
            if (r == 2) break;
            if (r == 0) next_fetch = 0;
            if (r == 1) {
                cfg.imperial = !cfg.imperial;
                config_save();
                next_fetch = 0;
            }
            dirty = true;
        }

        papp_datetime_t t;
        if (papp_local_time(&t) && t.minute != last_minute) {
            last_minute = t.minute;
            dirty = true;
        }
        if (dirty) {
            draw(false);
            dirty = false;
        }
        papp_disp_present_ext(surf, s_img);    /* also lets HOME open its menu */
        papp_delay_ms(100);
    }
    papp_disp_surface_destroy(surf);
    papp_free(s_img);
    return 0;
}
