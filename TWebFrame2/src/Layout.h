#pragma once

#include "CSS.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

struct RasterImageFrame;
struct RasterImage;
struct TableGridModel;

struct LayoutRect {
    float x = 0, y = 0, width = 0, height = 0;
    bool Contains(float px, float py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

// Object bounds in SVG user coordinates. The queried element's own transform
// is excluded, while transforms on its descendants participate in the bounds.
bool SvgObjectBoundingBox(const std::shared_ptr<Node>& node,StyleSheet& styles,
                          LayoutRect& bounds,ID2D1Factory* factory=nullptr,
                          IDWriteFactory* writeFactory=nullptr);
// Shaped text advance in SVG user units, before transforms or monitor scaling.
bool SvgTextAdvanceLength(const std::shared_ptr<Node>& node,StyleSheet& styles,
                          float& length,IDWriteFactory* writeFactory=nullptr,
                          std::vector<LayoutRect>* characters=nullptr);

struct LayoutBox {
    std::shared_ptr<Node> node;
    // Generated CSS boxes keep their originating DOM node separate so they
    // can participate in layout/painting without becoming DOM children.
    std::shared_ptr<Node> generatedFrom;
    std::wstring pseudo;
    LayoutBox* parent = nullptr;
    ComputedStyle style;
    LayoutRect rect;
    LayoutRect content;
    // DOM bounds for an inline split around block children. Relative offsets
    // survive ancestor scroll/position translation without walking children.
    bool splitInlineRectValid = false;
    LayoutRect splitInlineRectOffsets;
    std::vector<LayoutRect> splitInlinePaintOffsets;
    mutable bool inFlowBlockChildrenValid = false, inFlowBlockChildren = false;
    bool inlineContinuationValid = false;
    float inlineContinuationX = 0, inlineContinuationY = 0, inlineContinuationHeight = 0;
    // Relative offsets are applied after normal flow. Keep only the affected
    // direct children so ordinary containers do not scan again on relayout.
    std::vector<LayoutBox*> relativeChildren;
    bool definiteContentHeight = false;
    float relativeOffsetX = 0, relativeOffsetY = 0;
    // Stable gutters and forced vertical bars are resolved with the containing
    // block. Reuse their physical insets for child layout and CSSOM sizes.
    bool verticalGutterReserved = false;
    float scrollbarGutterLeft = 0, scrollbarGutterRight = 0;
    bool horizontalGutterReserved = false;
    float scrollbarGutterBottom = 0;
    float autoScrollbarHeightExpansion = 0;
    // Store the rectangular child clip relative to rect. Scroll/sticky moves
    // then reuse it without recomputing scrollbar pseudo styles per paint.
    bool overflowClipValid = false, overflowFlagsValid = false, clipsOverflow = false;
    LayoutRect overflowClipOffsets;
    // Half of the winning collapsed edge belongs to each adjacent cell.
    // Keep used widths separate from the authored computed border styles.
    mutable bool collapsedBordersResolved = false;
    mutable float collapsedTop = 0, collapsedRight = 0;
    mutable float collapsedBottom = 0, collapsedLeft = 0;
    // Share the table grid and resolved tracks across intrinsic measurement,
    // layout and border painting. Relayout invalidates this with other sizes.
    mutable std::shared_ptr<TableGridModel> tableGrid;
    // Conservative subtree geometry for rejecting off-screen stacking contexts
    // before walking their ancestor clips or testing individual descendants.
    LayoutRect subtreeBounds;
    std::vector<std::unique_ptr<LayoutBox>> children;
    // Painting and hit testing use the same CSS stacking order repeatedly.
    // Cache it after layout instead of rebuilding and sorting a temporary
    // vector for every frame and pointer event.
    std::vector<LayoutBox*> paintChildren;
    std::vector<LayoutBox*> nonNegativeStackingContexts;
    std::vector<LayoutBox*> verticallyOrderedChildren;
    std::vector<LayoutBox*> overlayChildren;
    const LayoutBox* deferredStackingScope = nullptr;
    bool establishesStackingContext = false;
    // Scroll changes positions, not styles or tree membership. Cache the boxes
    // to translate while constructing traversal metadata, including text runs.
    std::vector<LayoutBox*> scrollTranslationBoxes;
    std::vector<LayoutBox*> scrollStickyChildren;
    bool scrollTraversalCached = false;
    bool visible = true;
    bool preserveLeadingWhitespace = false;
    bool preserveTrailingWhitespace = false;
    bool trimLeadingLineWhitespace = false;
    bool inlineTextPositioned = false;
    bool textFlowFragment = false;
    bool inlineFlowFragments = false;
    bool containsSticky = false;
    bool stickyFlowYValid = false;
    float stickyFlowY = 0.0f;
    float scrollWidth = 0.0f;
    float scrollHeight = 0.0f;
    bool textareaVerticalScrollbar = false, textareaHorizontalScrollbar = false;
    // CSSOM sizes include padding and integer CSS-pixel rounding. Cache the
    // descendant overflow once per layout; geometry reads must not walk a
    // large subtree for each queried element.
    float cssOverflowLeft = 0.0f, cssOverflowRight = 0.0f, cssOverflowBottom = 0.0f;
    float cssScrollWidth = 0.0f, cssScrollHeight = 0.0f;
    float appliedScrollLeft = 0.0f;
    float appliedScrollTop = 0.0f;
    // The document root remains the authored body box for normal-flow
    // geometry, while its overflow is propagated to the browsing-context
    // viewport. Keep that scrollport separate so body margins do not pull the
    // page scrollbar away from the client edge.
    bool viewportScrollContainer = false;
    float viewportGutterLeft = 0, viewportGutterRight = 0, viewportGutterBottom = 0;
    LayoutRect viewportScrollport;
    std::wstring viewportOverflowX;
    std::wstring viewportOverflowY;
    // Synthetic line fragments retain their offset in the originating DOM
    // text node so pointer/caret geometry maps back to DOM offsets.
    size_t textSourceOffset = 0;
    std::wstring textLayoutKey;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
    mutable bool naturalWidthValid = false;
    mutable float naturalWidth = 0.0f;
    mutable bool minimumWidthValid = false;
    mutable float minimumWidth = 0.0f;
    mutable bool intrinsicMinimumWidthValid = false;
    mutable float intrinsicMinimumWidth = 0.0f;
    // Content measurements exclude the element's own preferred/min/max size.
    // Descendant contributions still include their authored size constraints.
    mutable bool maxContentWidthValid = false;
    mutable float maxContentWidth = 0.0f;
    mutable bool minContentWidthValid = false;
    mutable float minContentWidth = 0.0f;
    mutable bool naturalHeightValid = false;
    mutable float naturalHeightReference = 0.0f;
    mutable float naturalHeight = 0.0f;
    // Floats in ordinary descendants remain part of the nearest enclosing
    // block formatting context, even through a fixed-height visible-overflow box.
    mutable float naturalFloatBottom = 0.0f;
    mutable bool blockMarginsValid = false;
    mutable float blockMarginsReference = 0.0f;
    mutable float marginTopPositive = 0.0f, marginTopNegative = 0.0f;
    mutable float marginBottomPositive = 0.0f, marginBottomNegative = 0.0f;
    mutable bool collapseFirstChildMargin = false;
    mutable bool collapseLastChildMargin = false;
    mutable bool selfCollapsingMargins = false;
};

struct StyleTransition {
    std::wstring property;
    std::wstring from;
    std::wstring to;
    float elapsedMs = 0.0f;
    float durationMs = 0.0f;
    float delayMs = 0.0f;
    float x1 = 0.25f;
    float y1 = 0.1f;
    float x2 = 0.25f;
    float y2 = 1.0f;
    bool discrete = false;
};

struct StyleAnimation {
    std::wstring name,signature,direction=L"normal",fill=L"none";
    std::vector<CssKeyframe> frames;
    float elapsedMs=0,durationMs=0,delayMs=0,iterations=1;
    float x1=.25f,y1=.1f,x2=.25f,y2=1;
    bool paused=false;
};

class LayoutEngine {
public:
    using RasterImageResolver = std::function<std::shared_ptr<RasterImage>(const std::wstring&)>;
    using FramePainter = std::function<void(ID2D1RenderTarget*, const LayoutBox&)>;

    LayoutEngine(Document& document, StyleSheet& styleSheet);
    ~LayoutEngine();
    void SetRasterImageResolver(RasterImageResolver resolver);
    void SetFramePainter(FramePainter painter) { framePainter_ = std::move(painter); }
    void Layout(float width, float height, float deviceScale = 1.0f);
    // Reuse the current style/layout tree for viewport-only changes. A media
    // query boundary crossing automatically falls back to a full rebuild.
    void Relayout(float width, float height, float deviceScale = 1.0f);
    void Paint(ID2D1RenderTarget* target, IDWriteFactory* writeFactory,
               const LayoutRect* dirtyBounds = nullptr);
    bool ScrollAt(float x, float y, float wheelDelta,
                  std::shared_ptr<Node>* scrolledNode = nullptr,
                  bool horizontal = false, bool* scrollChainStopped = nullptr);
    bool SyncScroll(const std::shared_ptr<Node>& node);
    bool BeginScrollbarInteraction(float x, float y, std::shared_ptr<Node>& dragNode,
                                   float& dragOffset, bool& horizontal);
    bool BeginScrollbarInteraction(float x, float y, std::shared_ptr<Node>& dragNode,
                                   float& dragOffset);
    bool DragScrollbar(const std::shared_ptr<Node>& node, float x, float y,
                       float dragOffset, bool horizontal);
    bool DragScrollbar(const std::shared_ptr<Node>& node, float y, float dragOffset);
    bool Restyle(const std::shared_ptr<Node>& node, bool* geometryChanged = nullptr);
    std::shared_ptr<Node> HitTest(float x, float y) const;
    bool HitTestText(const std::shared_ptr<Node>& scope, float x, float y,
                     std::shared_ptr<Node>& textNode, size_t& textOffset);
    bool TextRangeRects(const std::shared_ptr<Node>& textNode, size_t textStart,
                        size_t textLength, std::vector<LayoutRect>& rects);
    bool VerticalCaretPosition(const std::shared_ptr<Node>& scope,
                               const std::shared_ptr<Node>& currentNode,
                               size_t currentOffset, float preferredX, bool upward,
                               std::shared_ptr<Node>& targetNode, size_t& targetOffset);
    bool TextCaretRect(const std::shared_ptr<Node>& textNode, size_t textOffset,
                       LayoutRect& caretRect);
    std::wstring DumpJson(bool includeText=false) const;
    std::wstring DumpRenderingTextJson() const;
    ComputedStyle StyleForRendering(const std::shared_ptr<Node>& node) const;
    std::wstring DumpRenderingStyleJson(const ComputedStyle& style) const;
    struct ElementSizes { float clientWidth=0,clientHeight=0,scrollWidth=0,scrollHeight=0; };
    ElementSizes ReadElementSizes(const std::shared_ptr<Node>& node) const;
    bool ReadElementRect(const std::shared_ptr<Node>& node,LayoutRect& rect) const;
    const LayoutBox* Root() const { return root_.get(); }
    const LayoutBox* BoxFor(const std::shared_ptr<Node>& node) const;
    bool VisualBounds(const std::shared_ptr<Node>& node, LayoutRect& bounds) const;
    bool HasActiveTransitions() const;
    bool AdvanceTransitions(float milliseconds);
    void ClearTransitions();
    void DiscardDeviceResources();

private:
    struct FirstLetterRun {
        size_t offset = 0;
        size_t length = 0;
        ComputedStyle style;
    };

    std::unique_ptr<LayoutBox> Build(const std::shared_ptr<Node>& node,
                                     const ComputedStyle* parentStyle,
                                     std::uint64_t parentContext = 1469598103934665603ull,
                                     size_t siblingIndex = 0, size_t siblingCount = 0,
                                     const std::shared_ptr<Node>& previousElement = {});
    void LayoutBoxTree(LayoutBox& box, const LayoutRect& available, bool forcedSize = false,
                       bool definiteWidth = true, bool definiteHeight = true);
    void LayoutRoot();
    void FinalizeScroll(LayoutBox& box);
    void LayoutBlock(LayoutBox& box, bool definiteHeight = true);
    void LayoutColumns(LayoutBox& box);
    void LayoutFlex(LayoutBox& box);
    void LayoutGrid(LayoutBox& box, bool definiteWidth, bool definiteHeight);
    void LayoutTable(LayoutBox& box);
    void UpdateTraversalMetadata(LayoutBox& box);
    void UpdateStackingContexts(LayoutBox& scope);
    void UpdateTopLayer();
    void PaintStackingContext(ID2D1RenderTarget* target, IDWriteFactory* factory,
                              LayoutBox& box, const LayoutRect& clipBounds);
    void PaintDialogBackdrop(ID2D1RenderTarget* target, const LayoutBox& dialog,
                             const LayoutRect& clipBounds);
    void PaintBox(ID2D1RenderTarget* target, IDWriteFactory* factory, LayoutBox& box,
                  const LayoutRect& clipBounds,
                  const LayoutBox* deferredScope = nullptr, bool paintDeferredRoot = false);
    void PaintScrollbars(ID2D1RenderTarget* target, const LayoutBox& box);
    std::shared_ptr<Node> HitTestStackingContext(const LayoutBox& box, float x, float y) const;
    std::shared_ptr<Node> HitTestBox(const LayoutBox& box, float x, float y, bool transformApplied = false) const;
    bool ScrollBox(LayoutBox& box, float x, float y, float wheelDelta,
                   std::shared_ptr<Node>* scrolledNode, bool horizontal);
    bool BeginScrollbarBox(LayoutBox& box, float x, float y,
                           std::shared_ptr<Node>& dragNode, float& dragOffset,
                           bool& horizontal);
    void DumpBox(const LayoutBox& box, std::wstring& output, bool& first,bool includeText) const;
    bool RestyleBox(LayoutBox& box, const ComputedStyle* parentStyle,
                    LayoutBox* localizedRoot = nullptr,
                    bool* fixedOffsetOnly = nullptr);
    void ApplyTransitions(LayoutBox& box);
    void RefreshTransitionFrame(LayoutBox& box);
    void ApplyAnimations(LayoutBox& box,bool updateDefinitions);
    void InvalidateMeasurements(LayoutBox& box);
    ID2D1SolidColorBrush* SolidBrush(ID2D1RenderTarget* target, unsigned int color);
    ID2D1SolidColorBrush* SolidBrush(ID2D1RenderTarget* target, const D2D1_COLOR_F& color);
    void EnsureGeometryResources(ID2D1RenderTarget* target);

    Document& document_;
    StyleSheet& styleSheet_;
    std::unique_ptr<LayoutBox> root_;
    FastMap<const Node*, LayoutBox*> boxIndex_;
    FastMap<const Node*, FirstLetterRun> firstLetterRuns_;
    FastMap<std::uint64_t, ComputedStyle> styleCache_;
    FastMap<const Node*, ComputedStyle> transitionTargets_;
    FastMap<const Node*, std::vector<StyleTransition>> transitions_;
    FastMap<const Node*, ComputedStyle> animationTargets_;
    FastMap<const Node*, std::vector<StyleAnimation>> animations_;
    std::uint64_t styleCacheVersion_ = 0;
    float viewportWidth_ = 0;
    float viewportHeight_ = 0;
    float deviceScale_ = 1.0f;
    std::vector<LayoutBox*> modalBoxes_;
    const LayoutBox* paintingTopLayer_ = nullptr;
    const LayoutBox* canvasBackgroundBox_ = nullptr;
    const LayoutBox* canvasHtmlBox_ = nullptr;
    const LayoutBox* canvasBodyBox_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> brushCacheTarget_;
    FastMap<unsigned int, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>> brushCache_;
    ID2D1Factory* geometryFactory_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> selectArrowGeometry_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> verticalArrowGeometry_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> horizontalArrowGeometry_;
    FastMap<std::wstring, Microsoft::WRL::ComPtr<ID2D1PathGeometry>> svgGeometryCache_;
    FastMap<std::wstring, std::shared_ptr<Node>> svgBackgroundCache_;
    RasterImageResolver rasterImageResolver_;
    FramePainter framePainter_;
    ID2D1RenderTarget* imageBitmapCacheTarget_ = nullptr;
    FastMap<const RasterImageFrame*, Microsoft::WRL::ComPtr<ID2D1Bitmap>> imageBitmapCache_;
    ID2D1RenderTarget* shadowBitmapCacheTarget_ = nullptr;
    FastMap<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> shadowBitmapCache_;
};

} // namespace TWebFrame::Internal
