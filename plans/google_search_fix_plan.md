# Google Search and Browser Fix Plan

## Problem Summary

The user reported three main issues with the Camel OS browser:
1. **Cannot type period (.) character** in the URL bar
2. **Clicking links on Google search results doesn't work** - shows "Click here to redirect" but nothing loads
3. **CSS is buggy** - pages don't render correctly

## Root Cause Analysis

### Issue 1: Period Character (.) Not Working

**Location**: [`usr/apps/browser_cdl.c:2525`](usr/apps/browser_cdl.c:2525)

**Root Cause**: The Delete key scan code `0x2E` conflicts with the ASCII code for period character `.` which is also `0x2E` (46 in decimal).

```c
case 0x2E: // Delete key - delete char at cursor
    cursor_delete();
    return;
```

When the user presses `.`, the code interprets it as the Delete key scan code instead of the period character. The input handling mixes scan codes with ASCII codes incorrectly.

**Solution**: Use proper keyboard scan code constants. The Delete key scan code is typically `0x53` on PC keyboards, not `0x2E`. The `0x2E` value appears to be a mistake. We need to:
1. Remove or fix the Delete key case to use the correct scan code
2. Allow period character (ASCII 46 / 0x2E) to be inserted normally

### Issue 2: Link Clicking Not Working

**Location**: [`usr/apps/browser_cdl.c:2836-2858`](usr/apps/browser_cdl.c:2836)

**Root Cause**: Multiple issues in the link click detection:

1. **Coordinate mismatch**: The mouse handler calculates link positions incorrectly:
   ```c
   int link_x = content_margin + lr->x;  // Uses content_margin = 10
   ```
   But the drawing code uses:
   ```c
   int draw_x = x + 10 + run->x;  // Uses x (window position) + 10
   ```

2. **Relative URLs not resolved**: Google search results use relative URLs like `/search?q=...` or `/url?q=...`. The browser doesn't resolve these relative to the current page URL.

3. **Google redirect URLs**: Google uses special redirect URLs that need to be parsed to extract the actual destination.

**Solution**:
1. Fix coordinate calculation in `on_mouse` to match drawing coordinates
2. Add URL resolution function to convert relative URLs to absolute URLs
3. Add special handling for Google redirect URLs

### Issue 3: CSS Rendering Issues

**Location**: [`usr/apps/browser_cdl.c:2696-2771`](usr/apps/browser_cdl.c:2696)

**Root Cause**: The CSS rendering has several issues:
1. Box runs are rendered but z-index ordering may be incorrect
2. Background colors may not render correctly for all elements
3. Text alignment and positioning may be off

**Solution**: Review and fix the rendering order and coordinate calculations.

---

## Detailed Fix Plan

### Fix 1: Period Character Input

**File**: `usr/apps/browser_cdl.c`

**Changes**:
```c
// Line 2525 - Remove or fix the Delete key case
// The scan code 0x2E is NOT the Delete key - Delete is typically 0x53
// 0x2E is the ASCII code for period character which should be handled by default case

// REMOVE THIS:
case 0x2E: // Delete key - delete char at cursor
    cursor_delete();
    return;

// OR REPLACE WITH CORRECT SCAN CODE:
case 0x53: // Delete key (correct scan code)
    cursor_delete();
    return;
```

### Fix 2: Link Click Coordinate Calculation

**File**: `usr/apps/browser_cdl.c`

**Changes in `on_mouse` function**:
```c
// Line 2836-2858 - Fix coordinate calculation
if (btn == 1) {
    int content_y = toolbar_y + 40;
    int scroll_offset = page_offset * 16;
    
    for (int i = 0; i < link_region_count; i++) {
        link_region_t* lr = &link_regions[i];
        // Fix: Match the drawing coordinates exactly
        // Drawing uses: draw_x = x + 10 + run->x
        // Mouse coordinates are relative to window, so we need:
        int link_y = content_y + lr->y - scroll_offset;
        int link_x = 10 + lr->x;  // Remove content_margin, use 10 directly
        
        if (my >= link_y && my <= link_y + lr->height &&
            mx >= link_x && mx <= link_x + lr->width) {
            
            // Resolve relative URLs before navigation
            char absolute_url[MAX_URL];
            resolve_url(lr->url, current_url, absolute_url);
            
            if (lr->target_blank) {
                open_url_new_tab(absolute_url);
            } else {
                sys->strcpy(current_url, absolute_url);
                fetch_url(current_url);
            }
            return;
        }
    }
}
```

### Fix 3: URL Resolution for Relative URLs

**File**: `usr/apps/browser_cdl.c`

**New function to add**:
```c
// Resolve relative URLs to absolute URLs
static void resolve_url(const char* href, const char* base_url, char* result) {
    // Already absolute URL
    if (sys->strstr(href, "://") || sys->strstr(href, "www.")) {
        sys->strcpy(result, href);
        return;
    }
    
    // Protocol-relative URL (//example.com/path)
    if (href[0] == '/' && href[1] == '/') {
        // Extract protocol from base URL
        char proto[16] = "https";
        const char* proto_end = sys->strstr(base_url, "://");
        if (proto_end) {
            int proto_len = proto_end - base_url;
            if (proto_len < 16) {
                sys->strncpy(proto, base_url, proto_len);
                proto[proto_len] = 0;
            }
        }
        sys->strcpy(result, proto);
        str_cat(result, ":");
        str_cat(result, href);
        return;
    }
    
    // Absolute path (/path)
    if (href[0] == '/') {
        // Extract origin from base URL
        char origin[MAX_URL];
        extract_origin(base_url, origin);
        sys->strcpy(result, origin);
        str_cat(result, href);
        return;
    }
    
    // Relative path (path or ./path)
    // Extract directory from base URL
    char base_dir[MAX_URL];
    sys->strcpy(base_dir, base_url);
    char* last_slash = sys->strrchr(base_dir, '/');
    if (last_slash && last_slash > base_dir + 8) {  // Keep protocol slashes
        *last_slash = 0;
    }
    
    // Handle ./ prefix
    if (href[0] == '.' && href[1] == '/') {
        href += 2;
    }
    
    sys->strcpy(result, base_dir);
    str_cat(result, "/");
    str_cat(result, href);
}

// Extract origin (protocol + host) from URL
static void extract_origin(const char* url, char* origin) {
    const char* proto = sys->strstr(url, "://");
    const char* start = proto ? proto + 3 : url;
    
    const char* path_start = sys->strchr(start, '/');
    
    if (path_start) {
        int origin_len = (path_start - url);
        sys->strncpy(origin, url, origin_len);
        origin[origin_len] = 0;
    } else {
        sys->strcpy(origin, url);
    }
}
```

### Fix 4: Google Redirect URL Handling

**File**: `usr/apps/browser_cdl.c`

**Add special handling for Google redirect URLs**:
```c
// Check if URL is a Google redirect and extract actual URL
static void extract_google_redirect(const char* url, char* result) {
    // Check for Google redirect URL pattern
    if (sys->strstr(url, "google.com/url?") || 
        sys->strstr(url, "google.com/search?")) {
        
        // Look for q= parameter
        const char* q_param = sys->strstr(url, "q=");
        if (q_param) {
            q_param += 2;  // Skip "q="
            
            // Extract URL until & or end
            int i = 0;
            while (q_param[i] && q_param[i] != '&' && i < MAX_URL - 1) {
                // URL decode %XX sequences
                if (q_param[i] == '%' && q_param[i+1] && q_param[i+2]) {
                    char hex[3] = {q_param[i+1], q_param[i+2], 0};
                    result[i] = (char)strtol(hex, 0, 16);
                    i++;
                    q_param += 2;
                } else if (q_param[i] == '+') {
                    result[i] = ' ';
                } else {
                    result[i] = q_param[i];
                }
                i++;
            }
            result[i] = 0;
            return;
        }
    }
    
    // Not a redirect, return original URL
    sys->strcpy(result, url);
}
```

---

## Implementation Order

1. **Fix period character input** (Quick fix, high impact)
   - Remove or correct the Delete key case

2. **Add URL resolution functions** (Foundation for link fix)
   - Add `resolve_url()` function
   - Add `extract_origin()` function
   - Add `extract_google_redirect()` function

3. **Fix link click coordinates** (Core fix)
   - Update `on_mouse()` coordinate calculation
   - Integrate URL resolution

4. **Test and verify**
   - Test typing URLs with periods
   - Test Google search and clicking results
   - Test various website navigation

---

## Testing Plan

### Test Case 1: Period Character
1. Open browser
2. Type `www.google.com` in URL bar
3. Verify period characters appear correctly
4. Press Enter to navigate

### Test Case 2: Google Search
1. Navigate to `https://www.google.com`
2. Type a search query and press Enter
3. Click on a search result link
4. Verify the destination page loads

### Test Case 3: Relative URLs
1. Navigate to a website with relative links
2. Click on relative links
3. Verify they resolve to correct absolute URLs

### Test Case 4: CSS Rendering
1. Navigate to various websites
2. Verify backgrounds, borders, and text render correctly

---

## Files to Modify

| File | Changes |
|------|---------|
| `usr/apps/browser_cdl.c` | Fix input handling, add URL resolution, fix mouse coordinates |

## Estimated Impact

- **Lines changed**: ~50-80 lines
- **New functions**: 3 (resolve_url, extract_origin, extract_google_redirect)
- **Risk level**: Low (isolated changes to specific functions)