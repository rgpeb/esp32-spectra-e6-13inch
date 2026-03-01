# Plan: Network Image Folder, Dithering Options, Image Cycling & Sleep Controls

## TODO (Parked for Later)
- **SD card support**: Pin sharing investigated, code written but mount fails on hardware. Needs physical debugging with logic analyser or oscilloscope.

---

## Overview

Four features to implement, with clear dependencies between them:

```
  1. Config & Storage       (foundation — other features depend on this)
      |
      ├── 2. HTTP Folder Image Source + Ordered Cycling
      ├── 3. Dithering Algorithm Selector
      └── 4. Timed Deep Sleep / Wake Interval
```

---

## Feature 1: Expand ApplicationConfig & Web Interface

### Why first
Every other feature needs new config fields stored in NVS and exposed in the web UI. Build the storage layer once, then each feature plugs into it.

### New config fields (ApplicationConfig.h)

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `folderUrl[300]` | char[] | `""` | HTTP URL to a directory listing page (replaces single imageUrl for folder mode) |
| `ditherMode` | uint8_t | `0` | 0 = Floyd-Steinberg, 1 = Atkinson, 2 = Ordered (Bayer 8x8), 3 = None (nearest) |
| `sleepMinutes` | uint16_t | `0` | Minutes between deep-sleep wakes. 0 = stay awake (current behaviour: 10-min server then permanent sleep) |
| `imageChangeMinutes` | uint16_t | `30` | How often (minutes) to advance to the next image in the folder |

### Separately in NVS (changes every cycle, not part of the main config blob)

| Key | Type | Purpose |
|-----|------|---------|
| `img_index` | uint16_t | Current position in the image list. Survives deep sleep. Wraps to 0 when it reaches the end. |

### Web interface additions (config.html)

Add a new **"Display Settings"** section between WiFi config and Upload:

```
┌─────────────────────────────────────┐
│       E-Ink Display Setup           │
├─────────────────────────────────────┤
│ WiFi Network:    [______________]   │
│ WiFi Password:   [______________]   │
│ Image URL:       [______________]   │  ← single image (existing)
│                                     │
│ ── Image Folder (optional) ──       │  ← NEW section
│ Folder URL:      [______________]   │
│   e.g. http://192.168.1.10/photos/  │
│   Serves an HTML directory listing  │
│   or a JSON array of filenames.     │
│                                     │
│ ── Display Settings ──              │  ← NEW section
│ Dithering:  [Floyd-Steinberg  ▼]    │
│ Change image every: [30 min   ▼]    │
│ Wake from sleep every: [1 hour ▼]   │
│                                     │
│ [Save Configuration]                │
├─────────────────────────────────────┤
│ Upload Local Image                  │
│ ...                                 │
└─────────────────────────────────────┘
```

New template placeholders: `{{CURRENT_FOLDER_URL}}`, `{{CURRENT_DITHER_MODE}}`, `{{CURRENT_SLEEP_MINUTES}}`, `{{CURRENT_IMAGE_CHANGE_MINUTES}}`

### ConfigurationServer changes
- `/save` endpoint: read the new form fields, populate config, persist to NVS
- Template processor: inject current values into dropdowns/fields

### Files changed
- `ApplicationConfig.h` — add fields
- `ApplicationConfigStorage.cpp` — NVS read/write of new fields + `img_index`
- `ConfigurationServer.cpp/.h` — new form fields, /save handler, template vars
- `data/config.html` — new HTML sections

---

## Feature 2: HTTP Folder Image Source + Ordered Cycling

### Approach: HTTP directory listing

**Why HTTP instead of SMB/CIFS?**
ESP32 has no production-quality SMB client library. HTTP is universal — works with any NAS (Synology, QNAP, TrueNAS all have built-in HTTP file serving), any `python3 -m http.server`, nginx `autoindex`, Apache, or even a Raspberry Pi. Zero extra software needed on most setups.

The user provides a `folderUrl` like `http://192.168.1.10/photos/`. The device:

1. **Fetches the directory page** (GET folderUrl)
2. **Parses image links** from the HTML response — extract all `href="..."` values ending in `.jpg`, `.jpeg`, `.png`, `.bmp`
3. **Sorts alphabetically** so the order is deterministic
4. **Reads `img_index` from NVS** — knows which image it's up to
5. **Downloads that image** (GET folderUrl + filename)
6. **Processes and displays it** using the existing decode/dither/render pipeline
7. **Increments `img_index`** in NVS (wraps to 0 at end of list)

### Priority order (ImageScreen::render)

```
1. LittleFS local image (uploaded via web UI)       ← highest, same as now
2. HTTP folder (if folderUrl is set)                 ← NEW
3. Single image URL (if imageUrl is set)             ← existing fallback
4. Nothing → show info screen
```

### Parsing strategy

Support two response formats so it works everywhere:

**Format A — HTML directory listing** (nginx autoindex, Apache, Python http.server):
```html
<a href="sunset.jpg">sunset.jpg</a>
<a href="mountains.png">mountains.png</a>
```
Parse: scan for `href="..."` where the value ends in an image extension.

**Format B — JSON array** (for advanced users who want explicit control):
```json
["sunset.jpg", "mountains.png", "night_sky.bmp"]
```
Parse: if response starts with `[`, treat as JSON array of filenames.

### New files
- `FolderImageSource.cpp/.h` — fetches directory, parses image list, downloads by index

### Modified files
- `ImageScreen.cpp/.h` — add `loadFromFolder()` between LittleFS and single-URL download
- `ApplicationConfigStorage.cpp` — read/write `img_index` to NVS

### Memory consideration
Only one image is in PSRAM at a time (same as current single-image flow). The directory listing is parsed into a simple array of filenames, then freed. Minimal extra memory.

---

## Feature 3: Dithering Algorithm Selector

### Algorithms to implement

| Mode | Name | Character | Best for |
|------|------|-----------|----------|
| 0 | **Floyd-Steinberg** | Smooth gradients, natural look | Photos, landscapes |
| 1 | **Atkinson** | Higher contrast, lighter midtones, crisper edges | E-ink displays (classic Mac aesthetic) |
| 2 | **Ordered (Bayer 8x8)** | Structured crosshatch pattern, no error diffusion | Fast rendering, stylised look |
| 3 | **None (nearest colour)** | Hard colour blocks, no dithering at all | Logos, flat illustrations, vector graphics |

### Implementation

The current `ditherImage()` in `ImageScreen.cpp` is a single Floyd-Steinberg implementation. Refactor into a strategy:

```cpp
std::unique_ptr<ColorImageBitmaps>
ImageScreen::ditherImage(uint16_t *rgb565Buffer, uint32_t width, uint32_t height) {
    switch (config.ditherMode) {
        case 1:  return ditherAtkinson(rgb565Buffer, width, height);
        case 2:  return ditherOrdered(rgb565Buffer, width, height);
        case 3:  return ditherNone(rgb565Buffer, width, height);
        default: return ditherFloydSteinberg(rgb565Buffer, width, height);
    }
}
```

### Atkinson dithering
Same structure as Floyd-Steinberg but different error diffusion kernel — only diffuses 6/8 (75%) of the error, which gives higher contrast and preserves whites better. Excellent for e-ink.

```
Error diffusion pattern:
         *   1/8  1/8
   1/8  1/8  1/8
        1/8
```

### Ordered (Bayer 8x8) dithering
Uses a threshold matrix instead of error diffusion. No error buffers needed — O(1) extra memory. Very fast.

```cpp
// For each pixel:
//   threshold = bayer8x8[y%8][x%8] * (255/64);
//   apply threshold to find nearest colour with bias
```

### None (nearest colour)
Simply `findNearestColor(r, g, b)` for each pixel. No error diffusion. Sharp colour boundaries.

### Files changed
- `ImageScreen.cpp` — refactor `ditherImage()` into 4 methods, add switch on `config.ditherMode`
- `ImageScreen.h` — declare new private methods

---

## Feature 4: Timed Deep Sleep & Wake Cycle

### Current behaviour
```
Boot → display image → run web server 10 min → permanent deep sleep (no wake)
```

### New behaviour
```
Boot → check if it's time to change image
     → if yes: advance img_index, fetch + display next image
     → if no: skip display refresh (save power + reduce e-ink wear)
     → run web server 10 min
     → deep sleep for N minutes (timer wakeup)
     → repeat
```

### Implementation

**In `main.cpp` setup(), replace permanent sleep with timed sleep:**
```cpp
if (appConfig->sleepMinutes > 0) {
    uint64_t sleepUs = (uint64_t)appConfig->sleepMinutes * 60ULL * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleepUs);
    printf("Deep sleep for %u minutes...\r\n", appConfig->sleepMinutes);
} else {
    printf("Permanent deep sleep (no timer). Reset to wake.\r\n");
}
esp_deep_sleep_start();
```

**Image change timing — use RTC memory counter:**
```cpp
RTC_DATA_ATTR static uint32_t minutesSinceLastChange = 0;
```

On each wake:
1. `minutesSinceLastChange += sleepMinutes`
2. If `minutesSinceLastChange >= imageChangeMinutes`:
   - Advance `img_index` in NVS
   - Reset counter to 0
   - Download and display next image
3. Else:
   - Skip display refresh entirely (saves power and e-ink wear)
   - Go straight to web server phase, then back to sleep

### Dropdown values for the web UI

**Wake from sleep every:**

| Label | Value (minutes) |
|-------|----------------|
| Never (manual reset only) | 0 |
| 15 minutes | 15 |
| 30 minutes | 30 |
| 1 hour | 60 |
| 2 hours | 120 |
| 4 hours | 240 |
| 6 hours | 360 |
| 12 hours | 720 |
| 24 hours | 1440 |

**Change image every:**

| Label | Value (minutes) |
|-------|----------------|
| Every wake | 0 |
| 15 minutes | 15 |
| 30 minutes | 30 |
| 1 hour | 60 |
| 2 hours | 120 |
| 6 hours | 360 |
| 12 hours | 720 |
| 24 hours | 1440 |

### Files changed
- `main.cpp` — replace permanent sleep with timed sleep, add image-change-interval logic
- `ApplicationConfig.h` — `sleepMinutes`, `imageChangeMinutes`

---

## Implementation Order

| Step | What | Depends on |
|------|------|-----------|
| 1 | Expand `ApplicationConfig` with all new fields | — |
| 2 | Update `ApplicationConfigStorage` for new fields + `img_index` | Step 1 |
| 3 | Update `config.html` with new UI sections (dropdowns, folder URL) | Step 1 |
| 4 | Update `ConfigurationServer` to read/save new fields from form | Steps 1–3 |
| 5 | Implement Atkinson, Ordered, and None dithering in `ImageScreen` | Step 1 |
| 6 | Implement `FolderImageSource` (directory parse + ordered download) | Steps 1–2 |
| 7 | Integrate folder source into `ImageScreen::render()` priority chain | Step 6 |
| 8 | Implement timed deep sleep + image change interval in `main.cpp` | Steps 1–2 |
| 9 | Test full cycle: wake → advance image → dither → display → sleep | All |

### File summary

| File | Action |
|------|--------|
| `ApplicationConfig.h` | Add `folderUrl`, `ditherMode`, `sleepMinutes`, `imageChangeMinutes` |
| `ApplicationConfigStorage.cpp/.h` | Persist new fields + separate `img_index` counter |
| `data/config.html` | Add folder URL field, dithering dropdown, two interval dropdowns |
| `ConfigurationServer.cpp/.h` | Handle new form fields in `/save`, template replacement |
| `ImageScreen.cpp/.h` | Refactor dithering into 4 algorithms, add `loadFromFolder()` |
| `FolderImageSource.cpp/.h` | **NEW** — HTTP directory fetch, parse, download-by-index |
| `main.cpp` | Timed sleep, image-change-interval logic, skip-refresh optimisation |
