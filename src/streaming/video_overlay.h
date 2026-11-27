/**
 * @file src/streaming/video_overlay.h
 * @brief Caches statistics, menus and docked keyboards for both YUY2 and RGB presentation.
 */
#pragma once

// standard includes
#include <memory>

// local includes
#include "src/_nxdk_compat/ffmpeg_memory.h"
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
   * @param width Text surface width.
   * @param height Text surface height.
   * @param bounds Available label rectangle.
   * @param shrinkToFit Shrink a single line with a one-pixel inset instead of cropping it.
   * @return Source and destination rectangles, empty for nonpositive dimensions.
   */
  OverlayLabelLayout layout_overlay_label(int width, int height, const VideoRectangle &bounds, bool shrinkToFit);

  /**
   * @brief Writable ARGB bitmap view with validated row storage.
   */
  struct OverlayBitmap {
    std::uint8_t *pixels;  ///< First destination byte.
    std::size_t capacity;  ///< Accessible destination byte count.
    int pitch;  ///< Byte stride between rows.
    int width;  ///< Pixel width.
    int height;  ///< Pixel height.
  };

  /**
   * @brief Circular menu geometry and clockwise highlighted sector.
   */
  struct MenuWheel {
    int centerX;  ///< Horizontal center in destination pixels.
    int centerY;  ///< Vertical center in destination pixels.
    int radius;  ///< Outer radius in pixels.
    int selected;  ///< Selected up/right/down/left sector, or minus one for none.
  };

  /**
   * @brief Rasterize all or one sector of an opaque circular menu onto ARGB storage.
   * @param target Destination bitmap, at most 2048 pixels in either dimension.
   * @param wheel Geometry with a radius between 1 and 2048, and its highlighted sector.
   * @param sector Repaint only this clockwise sector, or minus one for the whole ring.
   * @param clip Optional repaint rectangle restricting background restoration.
   * @return False for invalid storage or geometry, without changing destination pixels.
   */
  bool rasterize_menu_wheel(const OverlayBitmap &target, const MenuWheel &wheel, int sector = -1, const VideoRectangle *clip = nullptr);

  /**
   * @brief Placement of a shared panel inside the presented video rectangle.
   */
  enum class VideoOverlayPlacement {
    top_left,  ///< Statistics with the legacy safe-area inset.
    bottom_center,  ///< Centered bottom panel with the legacy safe-area inset.
    bottom_right,  ///< Lower-right indicator with a television-safe inset.
    dock_bottom,  ///< Centered keyboard flush with the bottom edge.
    dock_top,  ///< Centered keyboard flush with the top edge.
    full_screen,  ///< Full-stream overlay without an inset.
  };

  /**
   * @brief Choose the controls raster scale for the active output height.
   * @param height Output height in pixels.
   * @return Pixels per raster sample. A 1080i panel uses a half-size raster to fit alongside GPU video.
   */
  constexpr int controls_raster_scale(int height) {
    return height >= 1080 ? 2 : 1;
  }

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
     * @brief Share a tightly packed premultiplied SDL panel without allocating a second color bitmap.
     *
     * Publish only completed raster updates on the presentation thread. The owner must
     * report affected rectangles before drawing against mutated pixels.
     *
     * @param width Source width in pixels.
     * @param height Source height in pixels.
     * @param pixels Shared owner aliased to the first B, G, R, A byte.
     * @param capacity Accessible source byte count.
     * @param placement Panel anchor inside the video rectangle.
     * @param opaqueBackground Source was rendered onto an opaque panel.
     * @param scale Output pixels per raster pixel, either one or two.
     * @return False for invalid source storage, preserving the previous overlay.
     */
    bool share_argb_surface(int width, int height, std::shared_ptr<const std::uint8_t> pixels, std::size_t capacity, VideoOverlayPlacement placement, bool opaqueBackground, int scale = 1);
    /**
     * @brief Refresh the converted part of a shared surface after an in-place repaint.
     * @param region Updated rectangle in panel pixels.
     * @return True for a valid repaint of a shared panel.
     */
    bool update_shared_argb_surface(const VideoRectangle &region);
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
     * @brief Resolve the source crop for an already clipped output panel.
     * @param panel Panel bounds returned by rectangle, in output pixels.
     * @return Visible bitmap bounds before scaling.
     */
    VideoRectangle bitmap_rectangle(const VideoRectangle &panel) const;
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
     * @brief Write straight ARGB directly into a locked SDL texture region.
     * @param region Source rectangle in the panel.
     * @param pixels Destination address at the first pixel of the locked region.
     * @param pitch Destination row stride in bytes.
     * @return False for an invalid region or destination.
     */
    bool copy_argb_region(const VideoRectangle &region, std::uint8_t *pixels, int pitch) const;
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
     * @return True when the panel was copied or is empty. False for invalid storage or insufficient memory.
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
     * @brief Prepare geometry and storage for a YUY2 presentation cache.
     * @param frameWidth Video width.
     * @param frameHeight Video height.
     * @param destination Output video rectangle.
     * @param matrix Host color matrix.
     * @param rebuild Receives whether geometry or matrix changed.
     * @return False when the converted overlay cannot be allocated.
     */
    bool prepare_yuy2(int frameWidth, int frameHeight, const VideoRectangle &destination, VideoColorMatrix matrix, bool &rebuild);
    /**
     * @brief Convert two raster pixels into a packed YUY2 pair and optional coverage bytes.
     * @param bitmapRow Mapped source row.
     * @param firstBitmapColumn First mapped source column.
     * @param secondBitmapColumn Second mapped source column.
     * @param matrix Host color matrix.
     * @param packed Four writable YUY2 bytes.
     * @param coverage Two writable coverage bytes, or null when coverage is stored elsewhere.
     */
    void convert_yuy2_pair(int bitmapRow, int firstBitmapColumn, int secondBitmapColumn, VideoColorMatrix matrix, std::uint8_t *packed, std::uint8_t *coverage) const;
    /**
     * @brief Refresh only damaged YUY2 cache rows and columns.
     * @param matrix Host color matrix.
     * @param rebuild True to convert the entire overlay.
     */
    void update_yuy2(VideoColorMatrix matrix, bool rebuild);
    /**
     * @brief Blend the prepared YUY2 cache into validated framebuffer storage.
     * @param pixels Framebuffer storage.
     * @param pitch Framebuffer row stride.
     */
    void blend_yuy2(std::uint8_t *pixels, int pitch) const;

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
     * @brief Check whether a repaint rectangle fits wholly inside the panel.
     * @param region Proposed repaint.
     * @return True when a nonempty rectangle fits.
     */
    bool contains_region(const VideoRectangle &region) const;
    /**
     * @brief Merge a bounded repaint into the area due for conversion.
     * @param region Bounded repaint rectangle.
     */
    void mark_damage(const VideoRectangle &region);
    /**
     * @brief Convert source colors only when the software framebuffer's pixel depth changes.
     *
     * @param bpp RGB pixel depth.
     */
    void prepare_rgb(int bpp);

    /**
     * @brief Retain converted GPU colors, coverage and the geometry they describe.
     */
    struct Yuy2Cache {
      std::unique_ptr<std::uint8_t, decltype(&sunshine_ffmpeg_aligned_free)> bytes {nullptr, &sunshine_ffmpeg_aligned_free};  ///< Packed colors followed by optional coverage.
      std::size_t capacity = 0;  ///< Bytes available for colors and coverage.
      bool sharedCoverage = false;  ///< Read coverage directly from the retained raster.
      bool repeatRows = false;  ///< Each cached row serves two output rows at 2x.
      int height = 0;  ///< Number of physically stored rows.
      VideoRectangle bitmapBounds {};  ///< Visible source raster used to prepare the cache.
      VideoRectangle source {};  ///< Panel bounds in source luma pixels.
      int frameWidth = 0;  ///< Source frame width, or zero when the cache is invalid.
      int frameHeight = 0;  ///< Source frame height.
      int outputWidth = 0;  ///< Video output rectangle width.
      int outputHeight = 0;  ///< Video output rectangle height.
      VideoColorMatrix matrix = VideoColorMatrix::bt601;  ///< Host matrix used to encode the cache.
    };

    int width_ = 0;  ///< Packed panel width.
    int height_ = 0;  ///< Packed panel height.
    int scale_ = 1;  ///< Output pixels per raster pixel.
    VideoOverlayPlacement placement_ = VideoOverlayPlacement::top_left;  ///< Panel anchor shared by SDL, RGB and YUY2 paths.
    VideoRectangle damage_ {};  ///< Pending source-pixel changes; only affected YUY2 pairs need reconversion.
    std::vector<std::uint8_t> grayscale_;  ///< Unpremultiplied font and UI colors.
    std::vector<std::uint8_t> opacity_;  ///< Optional source coverage; empty for legacy opaque statistics.
    std::vector<std::uint8_t> argb_;  ///< Original straight ARGB UI colors; empty for grayscale statistics.
    std::shared_ptr<const std::uint8_t> sharedArgb_;  ///< Tightly packed premultiplied panel kept alive by its raster owner.
    bool opaque_ = true;  ///< Fully opaque panels use row-copy presentation paths.
    std::vector<std::uint8_t> rgb_;  ///< Cached RGB panel.
    int rgbBpp_ = 0;  ///< Pixel format of the RGB cache.
    Yuy2Cache yuy2_;  ///< GPU overlay cache and its associated geometry.
  };
}  // namespace streaming
