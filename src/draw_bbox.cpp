#include <cstring>
#include <cmath>
#include <algorithm>

// ============================================================================
// CORE STRUCTURES - Simplified from MuPDF
// ============================================================================

struct fz_irect {
    int x0, y0, x1, y1;
    
    bool is_empty() const {
        return x0 >= x1 || y0 >= y1;
    }
    
    bool is_valid() const {
        return x0 <= x1 && y0 <= y1;
    }
};

struct fz_matrix {
    float a, b, c, d, e, f;
};

struct fz_pixmap {
    int x, y;      // position
    int w, h;      // dimensions
    int n;         // components
    int alpha;
    int stride;
    unsigned char *samples;
    void *colorspace;
    void *seps;
    
    fz_irect bbox() const {
        return {x, y, x + w, y + h};
    }
};

struct fz_draw_state {
    fz_irect scissor;      // Clipping rectangle (bbox)
    fz_pixmap *dest;
    fz_pixmap *mask;
    fz_pixmap *shape;
    fz_pixmap *group_alpha;
    int blendmode;
    float alpha;
    fz_matrix ctm;
    int flags;
};

// ============================================================================
// RECTANGLE INTERSECTION - Core logic for bbox clipping
// ============================================================================

fz_irect fz_intersect_irect(const fz_irect &a, const fz_irect &b)
{
    fz_irect result;
    result.x0 = std::max(a.x0, b.x0);
    result.y0 = std::max(a.y0, b.y0);
    result.x1 = std::min(a.x1, b.x1);
    result.y1 = std::min(a.y1, b.y1);
    return result;
}

fz_irect fz_pixmap_bbox(const fz_pixmap *pix)
{
    if (!pix) return {0, 0, 0, 0};
    return pix->bbox();
}

// ============================================================================
// DRAW DEVICE INITIALIZATION WITH BBOX
// ============================================================================

class DrawDevice {
private:
    static constexpr int STACK_SIZE = 96;
    
    fz_matrix transform;
    int top;
    fz_draw_state *stack;
    int stack_cap;
    fz_draw_state init_stack[STACK_SIZE];
    
public:
    DrawDevice(const fz_matrix &transform, fz_pixmap *dest, const fz_irect *clip) 
        : transform(transform), top(0), stack_cap(STACK_SIZE)
    {
        stack = &init_stack[0];
        
        // Initialize bottom of stack with destination and bbox
        stack[0].dest = dest;
        stack[0].shape = nullptr;
        stack[0].group_alpha = nullptr;
        stack[0].mask = nullptr;
        stack[0].blendmode = 0;
        
        // Set initial scissor (clipping bbox) from destination
        stack[0].scissor.x0 = dest->x;
        stack[0].scissor.y0 = dest->y;
        stack[0].scissor.x1 = dest->x + dest->w;
        stack[0].scissor.y1 = dest->y + dest->h;
        
        // Apply additional clip bbox if provided
        if (clip) {
            apply_clip_bbox(clip);
        }
    }
    
    // Apply bbox clipping to current scissor
    void apply_clip_bbox(const fz_irect *clip) {
        if (!clip) return;
        
        fz_draw_state *state = &stack[top];
        
        if (clip->x0 > state->scissor.x0)
            state->scissor.x0 = clip->x0;
        if (clip->x1 < state->scissor.x1)
            state->scissor.x1 = clip->x1;
        if (clip->y0 > state->scissor.y0)
            state->scissor.y0 = clip->y0;
        if (clip->y1 < state->scissor.y1)
            state->scissor.y1 = clip->y1;
    }
    
    // Get current clipping bbox
    fz_irect get_scissor() const {
        return stack[top].scissor;
    }
    
    // Check if point is inside current bbox
    bool is_point_visible(int x, int y) const {
        const fz_irect &s = stack[top].scissor;
        return x >= s.x0 && x < s.x1 && y >= s.y0 && y < s.y1;
    }
    
    // Check if rect intersects with current bbox
    bool is_rect_visible(const fz_irect &rect) const {
        fz_irect clipped = fz_intersect_irect(rect, stack[top].scissor);
        return !clipped.is_empty();
    }
    
    // Get intersection of rect with current bbox
    fz_irect clip_rect(const fz_irect &rect) const {
        return fz_intersect_irect(rect, stack[top].scissor);
    }
};

// ============================================================================
// DRAWING OPERATIONS WITH BBOX CLIPPING
// ============================================================================

class DrawOperations {
public:
    // Fill path with bbox clipping
    static bool fill_path_with_bbox(
        DrawDevice &dev,
        const fz_irect &path_bbox)
    {
        // Get current scissor (clipping bbox)
        fz_irect scissor = dev.get_scissor();
        
        // Intersect path bbox with scissor
        fz_irect clipped = fz_intersect_irect(path_bbox, scissor);
        
        // If empty, skip drawing
        if (clipped.is_empty()) {
            return false;
        }
        
        // Draw only within clipped bbox
        return draw_within_bbox(clipped);
    }
    
    // Stroke path with bbox clipping
    static bool stroke_path_with_bbox(
        DrawDevice &dev,
        const fz_irect &path_bbox)
    {
        fz_irect scissor = dev.get_scissor();
        fz_irect clipped = fz_intersect_irect(path_bbox, scissor);
        
        if (clipped.is_empty()) {
            return false;
        }
        
        return draw_within_bbox(clipped);
    }
    
    // Fill image with bbox clipping
    static bool fill_image_with_bbox(
        DrawDevice &dev,
        const fz_pixmap *image,
        const fz_irect &image_bbox)
    {
        fz_irect dest_bbox = fz_pixmap_bbox(dev.get_current_dest());
        fz_irect scissor = dev.get_scissor();
        
        // Clip to destination and scissor
        fz_irect clip = fz_intersect_irect(dest_bbox, scissor);
        
        if (image->w == 0 || image->h == 0 || clip.is_empty()) {
            return false;
        }
        
        // Further clip to image bounds
        fz_irect final_clip = fz_intersect_irect(image_bbox, clip);
        
        if (final_clip.is_empty()) {
            return false;
        }
        
        return draw_image_within_bbox(image, final_clip);
    }
    
private:
    static bool draw_within_bbox(const fz_irect &bbox) {
        // Actual drawing implementation
        // This is where you'd rasterize/render within the bbox
        return true;
    }
    
    static bool draw_image_within_bbox(const fz_pixmap *image, const fz_irect &bbox) {
        // Actual image drawing implementation
        return true;
    }
    
    static fz_pixmap* get_current_dest();
};

// ============================================================================
// PUSH/POP CLIP STACK - For nested bbox clipping
// ============================================================================

class ClipStack {
private:
    DrawDevice &dev;
    
public:
    ClipStack(DrawDevice &d) : dev(d) {}
    
    // Push new clipping bbox onto stack
    void push_clip(const fz_irect &clip_bbox) {
        // In real implementation, this would:
        // 1. Push new state onto stack
        // 2. Intersect new bbox with current scissor
        // 3. Create mask pixmap if needed
        
        fz_irect current = dev.get_scissor();
        fz_irect new_scissor = fz_intersect_irect(current, clip_bbox);
        
        // Update scissor for new level
        // (simplified - real code manages full state stack)
    }
    
    // Pop clipping bbox from stack
    void pop_clip() {
        // Restore previous clipping state
        // Apply any accumulated mask
    }
};

// ============================================================================
// EXAMPLE USAGE
// ============================================================================

void example_draw_with_bbox() {
    // Create destination pixmap
    fz_pixmap dest;
    dest.x = 0;
    dest.y = 0;
    dest.w = 800;
    dest.h = 600;
    dest.n = 4; // RGBA
    dest.alpha = 1;
    dest.stride = dest.w * dest.n;
    dest.samples = new unsigned char[dest.stride * dest.h];
    
    // Define bbox to draw within (e.g., page rect or specific region)
    fz_irect draw_bbox = {100, 100, 500, 400};
    
    // Create draw device with bbox clipping
    fz_matrix identity = {1, 0, 0, 1, 0, 0};
    DrawDevice device(identity, &dest, &draw_bbox);
    
    // Now all drawing operations will be clipped to bbox [100,100,500,400]
    
    // Example: Draw a path
    fz_irect path_bbox = {50, 50, 600, 600};  // Larger than clip
    bool drawn = DrawOperations::fill_path_with_bbox(device, path_bbox);
    // Only region [100,100,500,400] will be affected
    
    // Check if specific point is visible
    bool visible = device.is_point_visible(250, 250);  // true - inside bbox
    bool outside = device.is_point_visible(50, 50);     // false - outside bbox
    
    delete[] dest.samples;
}

// ============================================================================
// KEY FUNCTIONS SUMMARY
// ============================================================================

/*
 * QUAN TRỌNG - CÁC HÀM CHÍNH:
 * 
 * 1. fz_intersect_irect() 
 *    - Tính giao của 2 rectangle
 *    - Dùng để clip bbox với scissor
 * 
 * 2. DrawDevice::apply_clip_bbox()
 *    - Áp dụng bbox clipping lên scissor hiện tại
 *    - Giới hạn vùng vẽ
 * 
 * 3. DrawDevice::get_scissor()
 *    - Lấy bbox clipping hiện tại
 * 
 * 4. DrawDevice::is_rect_visible()
 *    - Kiểm tra xem rectangle có giao với bbox không
 *    - Tối ưu: skip vẽ nếu không visible
 * 
 * 5. DrawDevice::clip_rect()
 *    - Clip rectangle với bbox hiện tại
 *    - Trả về phần giao
 * 
 * 6. DrawOperations::fill_*_with_bbox()
 *    - Các hàm vẽ có áp dụng bbox clipping
 *    - Chỉ vẽ trong vùng clip
 * 
 * LOGIC CHÍNH:
 * - Mọi thao tác vẽ đều intersect với scissor (bbox)
 * - Nếu kết quả empty -> skip
 * - Chỉ vẽ trong vùng giao
 */