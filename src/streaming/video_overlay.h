/**
 * @file src/streaming/video_overlay.h
 * @brief Caches statistics, menus and docked keyboards for both YUY2 and RGB presentation.
 */
#pragma once

#include "src/streaming/video_acceleration.h"

namespace streaming {
  /**
   * @brief Extract straight ARGB colors from a premultiplied Xbox surface without discarding SVG colors.
   * @param pixels Source bytes in little-endian B, G, R, A order.
   * @param capacity Accessible source bytes.
   * @param pitch Source row stride.
   * @param width Surface width, at most 2048.
   * @param height Surface height, at most 2048.
   * @param opaqueBackground Source was drawn onto an opaque panel; preserve colors and correct SDL's alpha rounding.
   * @param bitmap Optional reusable destination storage, avoiding a second full-screen allocation on repaint.
   * @return Unpremultiplied B, G, R, A bytes, or empty storage for invalid dimensions.
   */
  std::vector<std::uint8_t> extract_overlay_bitmap(const std::uint8_t *pixels, std::size_t capacity, int pitch, int width, int height, bool opaqueBackground = false, std::vector<std::uint8_t> bitmap = {});

  /**
   * @brief Source crop and destination for a centered overlay label.
   */
  struct OverlayLabelLayout {
    VideoRectangle source;  ///< Visible portion of the text surface.
    VideoRectangle destination;  ///< Bounded output rectangle.
  };

  /**
   * @brief Fit or crop centered text without changing SDL renderer clipping state.
   *
   * Standalone clip commands can crash nxdk's SDL 2.0.9 software renderer after
   * a texture flush. An explicit source crop also works on current native SDL.
   *
   * @param width Text surface width.
   * @param height Text surface height.
   * @param bounds Available label rectangle.
   * @param shrinkToFit Shrink a single line with a one-pixel inset instead of cropping it.
   * @return Source and destination rectangles, empty for nonpositive dimensions.
   */
  OverlayLabelLayout layout_overlay_label(int width, int height, const VideoRectangle &bounds, bool shrinkToFit);

  /**
   * @brief Rasterize a grayscale selection wheel directly into an ARGB panel.
   *
   * Avoid thousands of immediately flushed software-renderer commands per highlight.
   *
   * @param pixels ARGB8888 destination storage.
   * @param capacity Accessible bytes.
   * @param pitch Destination row stride.
   * @param width Panel width, at most 2048.
   * @param height Panel height, at most 2048.
   * @param centerX Wheel center X inside the panel.
   * @param centerY Wheel center Y inside the panel.
   * @param radius Outer radius in pixels, between 1 and 2048.
   * @param selected Selected clockwise sector starting at up, or -1 for none.
   * @param sector Repaint only this clockwise sector, or -1 for the whole ring.
   * @param clip Optional pixel rectangle restricting background restoration behind a sector label.
   * @return False when storage or geometry is invalid, without writing pixels.
   */
  bool rasterize_menu_wheel(std::uint8_t *pixels, std::size_t capacity, int pitch, int width, int height, int centerX, int centerY, int radius, int selected, int sector = -1, const VideoRectangle *clip = nullptr);

  /**
   * @brief Placement of a shared panel inside the presented video rectangle.
   */
  enum class VideoOverlayPlacement {
    top_left,  ///< Statistics and full-menu placement.
    bottom_center,  ///< Centered bottom panel with the legacy safe-area inset.
    dock_bottom,  ///< Centered keyboard flush with the bottom edge.
    dock_top,  ///< Centered keyboard flush with the top edge.
    full_screen,  ///< Full-stream overlay without an inset.
  };

  /**
   * @brief Colored UI or grayscale statistics with a shared anchor for every presentation path.
   */
  class VideoOverlay {
  public:
    /**
     * @brief Replace the panel, invalidate caches, and release converted storage when cleared.
     * @param width Bitmap width.
     * @param height Bitmap height.
     * @param pixels Packed grayscale bytes; invalid storage clears the panel.
     * @param placement Anchor inside the video rectangle.
     * @param opacity Optional per-pixel opacity; empty means fully opaque. Invalid storage clears the panel.
     */
    void set_bitmap(int width, int height, std::vector<std::uint8_t> pixels, VideoOverlayPlacement placement = VideoOverlayPlacement::top_left, std::vector<std::uint8_t> opacity = {});
    /**
     * @brief Replace colored pixels, retaining conversion caches and tracking damage for stable panel geometry.
     * @param width Bitmap width.
     * @param height Bitmap height.
     * @param pixels Packed B, G, R, A bytes; invalid storage clears the panel.
     * @param placement Anchor inside the video rectangle.
     */
    void set_argb_bitmap(int width, int height, std::vector<std::uint8_t> pixels, VideoOverlayPlacement placement = VideoOverlayPlacement::top_left);
    /**
     * @brief Copy a premultiplied SDL panel using the existing color buffer for same-size repaints.
     *
     * Release obsolete geometry and conversion caches before allocating a new panel. This avoids
     * overlapping full-screen copies while a background screenshot owns its capture buffers.
     *
     * @param width Source width, at most 2048.
     * @param height Source height, at most 2048.
     * @param pixels Premultiplied B, G, R, A source bytes, independent of this overlay's storage.
     * @param capacity Accessible source byte count.
     * @param pitch Source row stride.
     * @param placement Panel anchor inside the video rectangle.
     * @param opaqueBackground Source was rendered onto an opaque panel.
     * @return False for invalid source storage, preserving the previous overlay.
     */
    bool set_argb_surface(int width, int height, const std::uint8_t *pixels, std::size_t capacity, int pitch, VideoOverlayPlacement placement, bool opaqueBackground = false);
    /**
     * @brief Replace a bounded part of an existing colored panel without scanning or copying unchanged pixels.
     * @param region Rectangle in panel pixels; must be wholly inside the current panel.
     * @param pixels Tightly packed straight B, G, R, A bytes for the region.
     * @return True after updating pixels and invalidating the affected conversion cache, false for invalid input.
     */
    bool update_argb_bitmap(const VideoRectangle &region, const std::vector<std::uint8_t> &pixels);
    /**
     * @brief Resolve clipped panel bounds in output pixels for every presentation path.
     * @param destination Presented video rectangle.
     * @return Anchored panel rectangle, empty when the panel or available area is empty.
     */
    VideoRectangle rectangle(const VideoRectangle &destination) const;
    /**
     * @brief Get the panel width.
     *
     * @return Width, or zero for a cleared panel.
     */
    int width() const;
    /**
     * @brief Get the panel height.
     *
     * @return Height, or zero for a cleared panel.
     */
    int height() const;
    /**
     * @brief Get cached straight ARGB pixels for SDL alpha blending.
     *
     * @return Packed pixels.
     */
    const std::vector<std::uint8_t> &argb_pixels();
    /**
     * @brief Alpha-composite the panel into a validated packed YUY2 frame before GPU submission.
     *
     * Luma uses per-pixel coverage; shared chroma uses the mean coverage of each pixel pair.
     * @param pixels Destination frame.
     * @param capacity Available bytes.
     * @param pitch Row stride in bytes.
     * @param frameWidth Frame width.
     * @param frameHeight Frame height.
     * @param destination Output video rectangle controlling panel scale.
     * @param matrix Host video's color matrix used to encode UI colors.
     * @return True when the panel was copied or is empty.
     */
    bool draw_yuy2(std::uint8_t *pixels, std::size_t capacity, int pitch, int frameWidth, int frameHeight, const VideoRectangle &destination, VideoColorMatrix matrix = VideoColorMatrix::bt601);
    /**
     * @brief Composite the same panel into the final software framebuffer.
     *
     * Validate storage before writing and calculate byte offsets in size_t.
     *
     * @param pixels Destination storage.
     * @param capacity Available bytes.
     * @param pitch Row stride in bytes.
     * @param width Framebuffer width.
     * @param height Framebuffer height.
     * @param bpp Xbox RGB pixel depth: 15, 16 or 32.
     * @param destination Output video rectangle.
     * @return True when drawing completed or the panel is empty.
     */
    bool draw_rgb(std::uint8_t *pixels, std::size_t capacity, int pitch, int width, int height, int bpp, const VideoRectangle &destination);

    static constexpr int MARGIN = 8;  ///< Panel inset in output pixels.

  private:
    /**
     * @brief Resolve either source format to one straight ARGB pixel.
     * @param index Packed source pixel index.
     * @return ARGB color with its coverage in the high byte.
     */
    std::uint32_t color_at(std::size_t index) const;
    /**
     * @brief Invalidate converted colors and release their storage when the panel is empty.
     */
    void invalidate_caches();
    /**
     * @brief Convert source colors only when the software framebuffer's pixel depth changes.
     *
     * @param bpp RGB pixel depth.
     */
    void prepare_rgb(int bpp);
    int width_ = 0;  ///< Packed panel width.
    int height_ = 0;  ///< Packed panel height.
    VideoOverlayPlacement placement_ = VideoOverlayPlacement::top_left;  ///< Panel anchor shared by SDL, RGB and YUY2 paths.
    VideoRectangle damage_ {};  ///< Pending source-pixel changes; only affected YUY2 pairs need reconversion.
    std::vector<std::uint8_t> grayscale_;  ///< Unpremultiplied font and UI colors.
    std::vector<std::uint8_t> opacity_;  ///< Optional source coverage; empty for legacy opaque statistics.
    std::vector<std::uint8_t> argb_;  ///< Original straight ARGB UI colors; empty for grayscale statistics.
    bool opaque_ = true;  ///< Fully opaque panels use row-copy presentation paths.
    std::vector<std::uint8_t> rgb_;  ///< Cached RGB panel.
    int rgbBpp_ = 0;  ///< Pixel format of the RGB cache.
    std::vector<std::uint8_t> yuy2_;  ///< Cached source-resolution panel for scanout scaling.
    std::vector<std::uint8_t> yuy2Opacity_;  ///< Coverage scaled with the packed GPU panel.
    VideoRectangle source_ {};  ///< Panel bounds in source luma pixels.
    int frameWidth_ = 0;  ///< Source width used by the YUY2 cache.
    int frameHeight_ = 0;  ///< Source height used by the YUY2 cache.
    int outputWidth_ = 0;  ///< Output width used by the YUY2 cache.
    int outputHeight_ = 0;  ///< Output height used by the YUY2 cache.
    VideoColorMatrix matrix_ = VideoColorMatrix::bt601;  ///< Host matrix used to encode cached UI colors.
  };
}  // namespace streaming
