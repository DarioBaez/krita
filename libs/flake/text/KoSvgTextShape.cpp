/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2022 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KoSvgTextShape.h"

#include <QTextLayout>

#include <fontconfig/fontconfig.h>
#include <raqm.h>
#include FT_COLOR_H
#include FT_TRUETYPE_TABLES_H
#include FT_BITMAP_H
#include FT_OUTLINE_H
#include <graphemebreak.h>
#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>
#include <linebreak.h>

#include <klocalizedstring.h>

#include "KoSvgText.h"
#include "KoSvgTextProperties.h"
#include <KoDocumentResourceManager.h>
#include <KoShapeContainer_p.h>
#include <KoShapeController.h>
#include <text/KoCssTextUtils.h>
#include <text/KoFontRegistry.h>
#include <text/KoSvgTextChunkShape_p.h>
#include <text/KoSvgTextShapeMarkupConverter.h>
#include <text/KoPolygonUtils.h>

#include "kis_debug.h"
#include <kis_global.h>
#include "kis_algebra_2d.h"

#include <KoClipMaskPainter.h>
#include <KoColorBackground.h>
#include <KoIcon.h>
#include <KoPathShape.h>
#include <KoProperties.h>
#include <KoShapeLoadingContext.h>
#include <KoXmlNS.h>

#include <SvgLoadingContext.h>
#include <SvgGraphicContext.h>
#include <SvgUtil.h>
#include <SvgStyleWriter.h>

#include <QApplication>
#include <QFileInfo>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QThread>
#include <QtMath>
#include <memory>
#include <vector>

#include <boost/optional.hpp>

#include <text/KoSvgTextChunkShapeLayoutInterface.h>

#include <FlakeDebug.h>

#include <QSharedData>

using raqm_t_up = KisLibraryResourcePointer<raqm_t, raqm_destroy>;

enum BreakType {
    NoBreak,
    SoftBreak,
    HardBreak
};

enum LineEdgeBehaviour {
    NoChange, ///< Do nothing special.
    Collapse, ///< Collapse if first or last in line.
    HangBehaviour, ///< Hang at the start or end of line.
    ForceHang, ///< Force hanging at the end of line.
    ConditionallyHang ///< Only hang if necessary.
};

struct CharacterResult {
    QPointF finalPosition;
    qreal rotate = 0.0;
    bool hidden = false; // whether the character will be drawn.
    // we can't access characters that aren't part of a typographic character
    // so we're setting 'middle' to true and addressable to 'false'.
    // The original svg specs' notion of addressable character relies on utf16,
    // and it's suggested to have it per-typographic character.
    // https://github.com/w3c/svgwg/issues/537
    bool addressable = true; // whether the character is not discarded for various reasons.
    bool middle = false; // whether the character is the second of last of a
                         // typographic character.
    bool anchored_chunk = false; // whether this is the start of a new chunk.

    QPainterPath path;
    QImage image{nullptr};

    QVector<QPainterPath> colorLayers;
    QVector<QBrush> colorLayerColors;
    QVector<bool> replaceWithForeGroundColor;

    QRectF boundingBox;
    int visualIndex = -1;
    QPointF cssPosition = QPointF();
    QPointF advance;
    BreakType breakType = NoBreak;
    LineEdgeBehaviour lineEnd = NoChange;
    LineEdgeBehaviour lineStart = NoChange;
    bool justifyBefore = false;///< Justification Opportunity precedes this character.
    bool justifyAfter = false; ///< Justification Opportunity follows this character.
    bool isHanging = false;
    bool textLengthApplied = false;
    bool overflowWrap = false;

    qreal halfLeading;
    qreal ascent;
    qreal descent;
    QFont::Style fontStyle = QFont::StyleNormal;
    int fontWeight = 400;

    KoSvgText::TextAnchor anchor = KoSvgText::AnchorStart;
    KoSvgText::Direction direction = KoSvgText::DirectionLeftToRight;

    QTransform finalTransform() const {
        QTransform tf =
            QTransform::fromTranslate(finalPosition.x(), finalPosition.y());
        tf.rotateRadians(rotate);
        return tf;
    }
};

struct LineChunk {
    QLineF length;
    QVector<int> chunkIndices;
    QRectF boundingBox;
};

/**
 * @brief The LineBox struct
 *
 * The line box struct is to simplify keeping track of lines inside the wrapping
 * functions. It somewhat corresponds to CSS line boxes, with the caveat that formally,
 * a line split in two in CSS/SVG would be two line boxes, while we instead have two
 * line chunks in a single line box. This is necessary to ensure we can calculate the
 * same line height for boxes split by a shape.
 *
 * CSS-Inline-3 defines Line Boxes here: https://www.w3.org/TR/css-inline-3/#line-box
 * CSS-Text-3 briefly talks about them here: https://www.w3.org/TR/css-text-3/#bidi-linebox
 * SVG-2 chapter text talks about them here: https://svgwg.org/svg2-draft/text.html#TextLayoutAutoNotes
 *
 * What is important to us is that all the above specifications, when they talk about Bidi-reordering,
 * agree that the order is dependant on the paragraph/block level direction, and is not affected by
 * the inline content changing direction. Which is good, because that'd make things super hard.
 */
struct LineBox {

    LineBox() {
    }

    LineBox(QPointF start, QPointF end) {
        LineChunk chunk;
        chunk.length =  QLineF(start, end);
        chunks.append(chunk);
        currentChunk = 0;
    }

    LineBox(QVector<QLineF> lineWidths, bool ltr, QPointF indent) {
        textIndent = indent;
        if (ltr) {
            Q_FOREACH(QLineF line, lineWidths) {
                LineChunk chunk;
                chunk.length = line;
                chunks.append(chunk);
                currentChunk = 0;
            }
        } else {
            Q_FOREACH(QLineF line, lineWidths) {
                LineChunk chunk;
                chunk.length = QLineF(line.p2(), line.p1());
                chunks.insert(0, chunk);
                currentChunk = 0;
            }
        }
    }

    QVector<LineChunk> chunks;
    int currentChunk = -1;

    qreal expectedLineTop = 0;
    qreal actualLineTop = 0;
    qreal actualLineBottom = 0;

    QPointF baselineTop = QPointF();
    QPointF baselineBottom = QPointF();

    QPointF textIndent = QPointF();
    bool firstLine = false;
    bool lastLine = false;
    bool lineFinalized = false;
    bool justifyLine = false;

    LineChunk chunk() {
        return chunks.value(currentChunk);
    }

    void setCurrentChunk(LineChunk chunk) {
        currentChunk = qMax(currentChunk, 0);
        if (currentChunk < chunks.size()) {
            chunks[currentChunk] = chunk;
        } else {
            chunks.append(chunk);
        }
    }

    void clearAndAdjust(bool isHorizontal, QPointF current, QPointF indent) {
        actualLineBottom = 0;
        actualLineTop = 0;
        LineChunk chunk;
        textIndent = indent;
        QLineF length = chunks.at(currentChunk).length;
        if (isHorizontal) {
            length.setP1(QPointF(length.p1().x(), current.y()));
            length.setP2(QPointF(length.p2().x(), current.y()));
        } else {
            length.setP1(QPointF(current.x(), length.p1().y()));
            length.setP2(QPointF(current.x(), length.p2().y()));
        }
        chunks.clear();
        currentChunk = 0;
        chunk.length = length;
        chunks.append(chunk);
        firstLine = false;
    }

    void setCurrentChunkForPos(QPointF pos, bool isHorizontal) {
        for (int i=0; i<chunks.size(); i++) {
            LineChunk chunk = chunks.at(i);
            if (isHorizontal) {
                if ((pos.x() < qMax(chunk.length.p1().x(), chunk.length.p2().x())) &&
                        (pos.x() >= qMin(chunk.length.p1().x(), chunk.length.p2().x()))) {
                        currentChunk = i;
                        break;
                }
            } else {
                if ((pos.y() < qMax(chunk.length.p1().y(), chunk.length.p2().y())) &&
                        (pos.y() >= qMin(chunk.length.p1().y(), chunk.length.p2().y()))) {
                        currentChunk = i;
                        break;
                }
            }
        }
    }

    bool isEmpty() {
        if (chunks.isEmpty()) return true;
        return chunks.at(currentChunk).chunkIndices.isEmpty();
    }
};

class KoSvgTextShape::Private
{
public:
    // NOTE: the cache data is shared between all the instances of
    //       the shape, though it will be reset locally if the
    //       accessing thread changes

    Private() = default;

    Private(const Private &rhs) {
        Q_FOREACH (KoShape *shape, rhs.shapesInside) {
            KoShape *clonedShape = shape->cloneShape();
            KIS_ASSERT_RECOVER(clonedShape) { continue; }

            shapesInside.append(clonedShape);
        }
        Q_FOREACH (KoShape *shape, rhs.shapesSubtract) {
            KoShape *clonedShape = shape->cloneShape();
            KIS_ASSERT_RECOVER(clonedShape) { continue; }

            shapesSubtract.append(clonedShape);
        }
        textRendering = rhs.textRendering;
        yRes = rhs.yRes;
        xRes = rhs.xRes;
        result = rhs.result;
        lineBoxes = rhs.lineBoxes;
    };

    TextRendering textRendering = Auto;
    int xRes = 72;
    int yRes = 72;
    QList<KoShape*> shapesInside;
    QList<KoShape*> shapesSubtract;

    QVector<CharacterResult> result;
    QVector<LineBox> lineBoxes;

    void clearAssociatedOutlines(const KoShape *rootShape);
    void resolveTransforms(const KoShape *rootShape, int &currentIndex, bool isHorizontal, bool textInPath, QVector<KoSvgText::CharTransformation> &resolved, QVector<bool> collapsedChars);
    static QPainterPath convertFromFreeTypeOutline(FT_GlyphSlotRec *glyphSlot);
    static QImage convertFromFreeTypeBitmap(FT_GlyphSlotRec *glyphSlot);
    static QVector<LineBox> breakLines(const KoSvgTextProperties &properties,
                           const QMap<int, int> &logicalToVisual,
                           QVector<CharacterResult> &result,
                           QPointF startPos);
    static QList<QPainterPath> getShapes(QList<KoShape*> shapesInside, QList<KoShape*> shapesSubtract, const KoSvgTextProperties &properties);
    static QVector<LineBox> flowTextInShapes(const KoSvgTextProperties &properties,
                                 const QMap<int, int> &logicalToVisual,
                                 QVector<CharacterResult> &result, QList<QPainterPath> shapes);
    void applyTextLength(const KoShape *rootShape, QVector<CharacterResult> &result, int &currentIndex, int &resolvedDescendentNodes, bool isHorizontal);
    static void applyAnchoring(QVector<CharacterResult> &result, bool isHorizontal);
    static qreal
    characterResultOnPath(CharacterResult &cr, qreal length, qreal offset, bool isHorizontal, bool isClosed);
    static QPainterPath stretchGlyphOnPath(const QPainterPath &glyph,
                                           const QPainterPath &path,
                                           bool isHorizontal,
                                           qreal offset,
                                           bool isClosed);
    static void applyTextPath(const KoShape *rootShape, QVector<CharacterResult> &result, bool isHorizontal);
    void computeFontMetrics(const KoShape *rootShape,
                            const QMap<int, int> &parentBaselineTable,
                            qreal parentFontSize,
                            QPointF superScript,
                            QPointF subScript,
                            QVector<CharacterResult> &result, QVector<LineBox> lineBoxes,
                            int &currentIndex,
                            qreal res,
                            bool isHorizontal);
    void computeTextDecorations(const KoShape *rootShape,
                                const QVector<CharacterResult>& result,
                                const QMap<int, int>& logicalToVisual,
                                qreal minimumDecorationThickness,
                                KoPathShape *textPath,
                                qreal textPathoffset,
                                bool side,
                                int &currentIndex,
                                bool isHorizontal,
                                bool ltr,
                                bool wrapping);
    void paintPaths(QPainter &painter,
                    const QPainterPath &outlineRect,
                    const KoShape *rootShape,
                    const QVector<CharacterResult> &result,
                    QPainterPath &chunk,
                    int &currentIndex);
    QList<KoShape *> collectPaths(const KoShape *rootShape, QVector<CharacterResult> &result, int &currentIndex);
};

KoSvgTextShape::KoSvgTextShape()
    : KoSvgTextChunkShape()
    , d(new Private)
{
    setShapeId(KoSvgTextShape_SHAPEID);
}

KoSvgTextShape::KoSvgTextShape(const KoSvgTextShape &rhs)
    : KoSvgTextChunkShape(rhs)
    , d(new Private(*rhs.d))
{
    setShapeId(KoSvgTextShape_SHAPEID);
}

KoSvgTextShape::~KoSvgTextShape()
{
}

KoShape *KoSvgTextShape::cloneShape() const
{
    return new KoSvgTextShape(*this);
}

void KoSvgTextShape::shapeChanged(ChangeType type, KoShape *shape)
{
    KoSvgTextChunkShape::shapeChanged(type, shape);

    if (type == StrokeChanged || type == BackgroundChanged || type == ContentChanged) {
        relayout();
    }
}

void KoSvgTextShape::paintComponent(QPainter &painter) const
{
    // if (d->textRendering == OptimizeLegibility) {
    /**
     * HACK ALERT:
     *
     * For hinting and bitmaps, we need to get the hinting metrics from
     * freetype, but those need the DPI. We can't get the DPI normally, however,
     * neither rotate and shear change the length of a line, and it may not be
     * that bad if freetype receives a scaled value for the DPI.
     */
    int xRes = qRound(painter.transform().map(QLineF(QPointF(), QPointF(72, 0))).length());
    int yRes = qRound(painter.transform().map(QLineF(QPointF(), QPointF(0, 72))).length());
    if (xRes != d->xRes || yRes != d->yRes) {
        d->xRes = xRes;
        d->yRes = yRes;
        relayout();
    }
    /*} else {
        if (72 != d->xRes || 72 != d->yRes) {
            d->xRes = 72;
            d->yRes = 72;
            relayout();
        }
    }*/
    painter.save();
    if (d->textRendering == OptimizeSpeed) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    } else {
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    }

    QPainterPath chunk;
    int currentIndex = 0;
    if (!d->result.isEmpty()) {
        d->paintPaths(painter, this->outline(), this, d->result, chunk, currentIndex);
    }
    /* Debug
    Q_FOREACH (KoShape *child, this->shapes()) {
        const KoSvgTextChunkShape *textPathChunk = dynamic_cast<const
    KoSvgTextChunkShape*>(child); if (textPathChunk) { painter.save();
            painter.setPen(Qt::magenta);
            painter.setOpacity(0.5);
            if (textPathChunk->layoutInterface()->textPath()) {
                QPainterPath p =
    textPathChunk->layoutInterface()->textPath()->outline(); p =
    textPathChunk->layoutInterface()->textPath()->transformation().map(p);
                painter.strokePath(p, QPen(Qt::green));
                painter.drawPoint(p.pointAtPercent(0));
                painter.drawPoint(p.pointAtPercent(p.percentAtLength(p.length()*0.5)));
                painter.drawPoint(p.pointAtPercent(1.0));
            }
            painter.restore();
        }
    }
    */
    /* Debug
    Q_FOREACH (KoShape *shapeInside, d->shapesInside) {
        QPainterPath p = shapeInside->outline();
        p = shapeInside->transformation().map(p);
        painter.strokePath(p, QPen(Qt::green));
    }
    Q_FOREACH (KoShape *shapeInside, d->shapesSubtract) {
        QPainterPath p = shapeInside->outline();
        p = shapeInside->transformation().map(p);
        painter.strokePath(p, QPen(Qt::red));
    }
    //*/
    /*
    Q_FOREACH(LineBox lineBox, d->lineBoxes) {
        Q_FOREACH(LineChunk chunk, lineBox.chunks) {
            painter.setBrush(QBrush(Qt::transparent));
            painter.setPen(QColor(0, 128, 255, 80));
            painter.drawLine(chunk.length);
            painter.setPen(QColor(255, 128, 0, 80));
            painter.drawRect(chunk.boundingBox);
        }
    }
    //*/

    painter.restore();
}

void KoSvgTextShape::paintStroke(QPainter &painter) const
{
    Q_UNUSED(painter);
    // do nothing! everything is painted in paintComponent()
}

QList<KoShape *> KoSvgTextShape::textOutline() const
{
    QList<KoShape *> shapes;
    int currentIndex = 0;
    if (!d->result.empty()) {
        shapes = d->collectPaths(this, d->result, currentIndex);
    }

    return shapes;
}

void KoSvgTextShape::setTextRenderingFromString(const QString &textRendering)
{
    if (textRendering == "optimizeSpeed") {
        d->textRendering = OptimizeSpeed;
    } else if (textRendering == "optimizeLegibility") {
        d->textRendering = OptimizeLegibility;
    } else if (textRendering == "geometricPrecision") {
        d->textRendering = GeometricPrecision;
    } else {
        d->textRendering = Auto;
    }
}

QString KoSvgTextShape::textRenderingString() const
{
    if (d->textRendering == OptimizeSpeed) {
        return "optimizeSpeed";
    } else if (d->textRendering == OptimizeLegibility) {
        return "optimizeLegibility";
    } else if (d->textRendering == GeometricPrecision) {
        return "geometricPrecision";
    } else {
        return "auto";
    }
}

void KoSvgTextShape::setShapesInside(QList<KoShape *> shapesInside)
{
    d->shapesInside = shapesInside;
}

QList<KoShape *> KoSvgTextShape::shapesInside() const
{
    return d->shapesInside;
}

void KoSvgTextShape::setShapesSubtract(QList<KoShape *> shapesSubtract)
{
    d->shapesSubtract = shapesSubtract;
}

QList<KoShape *> KoSvgTextShape::shapesSubtract() const
{
    return d->shapesSubtract;
}

QMap<QString, QString> KoSvgTextShape::shapeTypeSpecificStyles(SvgSavingContext &context) const
{
    QMap<QString, QString> map = this->textProperties().convertParagraphProperties();
    if (!d->shapesInside.isEmpty()) {
        QStringList shapesInsideList;
        Q_FOREACH(KoShape* shape, d->shapesInside) {
            QString id = SvgStyleWriter::embedShape(shape, context);
            shapesInsideList.append(QString("url(#%1)").arg(id));
        }
        map.insert("shape-inside", shapesInsideList.join(" "));
    }
    if (!d->shapesSubtract.isEmpty()) {
        QStringList shapesInsideList;
        Q_FOREACH(KoShape* shape, d->shapesSubtract) {
            QString id = SvgStyleWriter::embedShape(shape, context);
            shapesInsideList.append(QString("url(#%1)").arg(id));
        }
        map.insert("shape-subtract", shapesInsideList.join(" "));
    }

    return map;
}

void KoSvgTextShape::resetTextShape()
{
    KoSvgTextChunkShape::resetTextShape();
    relayout();
}

static QString glyphFormatToStr(const FT_Glyph_Format _v)
{
    const unsigned int v = _v;
    QString s;
    s += (v >> 24) & 0xFF;
    s += (v >> 16) & 0xFF;
    s += (v >> 8) & 0xFF;
    s += (v >> 0) & 0xFF;
    return s;
}

static void emboldenGlyphIfNeeded(raqm_glyph_t &currentGlyph, CharacterResult &charResult)
{
    if (charResult.fontWeight >= 600 && !(currentGlyph.ftface->style_flags & FT_STYLE_FLAG_BOLD)) {
        // This code is somewhat inspired by Firefox.
        FT_Pos strength =
            FT_MulFix(currentGlyph.ftface->units_per_EM, currentGlyph.ftface->size->metrics.y_scale) / 48;

        if (currentGlyph.ftface->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
            // This is similar to what FT_GlyphSlot_Embolden does.

            // Round down to full pixel.
            strength &= ~63;
            if (strength == 0) {
                // ... but it has to be at least one pixel.
                strength = 64;
            }

            FT_GlyphSlot_Own_Bitmap(currentGlyph.ftface->glyph);

            // Embolden less vertically than horizontally. Especially if
            // strength is only 1px, don't embolden vertically at all.
            // Otherwise it makes the glyph way too heavy, especially for
            // CJK glyphs in small sizes.
            const FT_Pos strengthY = strength - 64;
            FT_Bitmap_Embolden(currentGlyph.ftface->glyph->library,
                                &currentGlyph.ftface->glyph->bitmap,
                                strength,
                                strengthY);

            if (currentGlyph.x_advance != 0) {
                currentGlyph.x_advance += strength;
            }
            if (currentGlyph.y_advance != 0) {
                currentGlyph.y_advance -= strengthY;
            }
        } else {
            FT_Outline_Embolden(&currentGlyph.ftface->glyph->outline, strength);

            if (currentGlyph.x_advance != 0) {
                currentGlyph.x_advance += strength;
            }
            if (currentGlyph.y_advance != 0) {
                currentGlyph.y_advance -= strength;
            }
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void KoSvgTextShape::relayout() const
{
    d->clearAssociatedOutlines(this);

    // The following is based on the text-layout algorithm in SVG 2.
    KoSvgText::WritingMode writingMode = KoSvgText::WritingMode(this->textProperties().propertyOrDefault(KoSvgTextProperties::WritingModeId).toInt());
    KoSvgText::Direction direction = KoSvgText::Direction(this->textProperties().propertyOrDefault(KoSvgTextProperties::DirectionId).toInt());
    KoSvgText::AutoValue inlineSize = this->textProperties().propertyOrDefault(KoSvgTextProperties::InlineSizeId).value<KoSvgText::AutoValue>();
    QString lang = this->textProperties().property(KoSvgTextProperties::TextLanguage).toString().toUtf8();

    bool isHorizontal = writingMode == KoSvgText::HorizontalTB;

    FT_Int32 loadFlags = 0;

    if (d->textRendering == GeometricPrecision || d->textRendering == Auto) {
        // without load_no_hinting, the advance and offset will be rounded
        // to nearest pixel, which we don't want as we're using the vector
        // outline.
        loadFlags |= FT_LOAD_NO_HINTING;

        // Disable embedded bitmaps because they _do not_ follow geometric
        // precision, but is focused on legibility.
        // This does not affect bitmap-only fonts.
        loadFlags |= FT_LOAD_NO_BITMAP;
    } else {
        // When using hinting, sometimes the bounding box does not encompass the
        // drawn glyphs properly.
        // The default hinting works best for vertical, while the 'light'
        // hinting mode works best for horizontal.
        if (isHorizontal) {
            loadFlags |= FT_LOAD_TARGET_LIGHT;
        }
    }
    // Whenever the freetype docs talk about a 26.6 floating point unit, they
    // mean a 1/64 value.
    const qreal ftFontUnit = 64.0;
    const qreal ftFontUnitFactor = 1 / ftFontUnit;
    QTransform ftTF = QTransform::fromScale(ftFontUnitFactor, -ftFontUnitFactor);
    const qreal finalRes = qMin(d->xRes, d->yRes);
    const qreal scaleToPT = 72. / finalRes;
    const qreal scaleToPixel = finalRes / 72.;
    QTransform dpiScale = QTransform::fromScale(scaleToPT, scaleToPT);
    ftTF *= dpiScale;
    // Some fonts have a faulty underline thickness,
    // so we limit the minimum to be a single pixel wide.
    qreal minimumDecorationThickness = scaleToPT;

    // First, get text. We use the subChunks because that handles bidi for us.
    // SVG 1.1 suggests that each time the xy position of a piece of text
    // changes, that this should be seperately shaped, but according to SVGWG
    // issues 631 and 635 noone who actually uses bidi likes this, and it also
    // complicates the algorithm, so we're not doing that. Anchored Chunks will
    // get generated later. https://github.com/w3c/svgwg/issues/631
    // https://github.com/w3c/svgwg/issues/635

    bool first = false;
    QVector<KoSvgTextChunkShapeLayoutInterface::SubChunk> textChunks = layoutInterface()->collectSubChunks(false, first);
    QString text;
    Q_FOREACH (const KoSvgTextChunkShapeLayoutInterface::SubChunk &chunk, textChunks) {
        text.append(chunk.text);
    }
    debugFlake << "Laying out the following text: " << text;

    // 1. Setup.

    // KoSvgText::TextSpaceTrims trims =
    // this->textProperties().propertyOrDefault(KoSvgTextProperties::TextTrimId).value<KoSvgText::TextSpaceTrims>();
    KoSvgText::TextWrap wrap = KoSvgText::TextWrap(this->textProperties().propertyOrDefault(KoSvgTextProperties::TextWrapId).toInt());
    KoSvgText::TextSpaceCollapse collapse = KoSvgText::TextSpaceCollapse(this->textProperties().propertyOrDefault(KoSvgTextProperties::TextCollapseId).toInt());
    KoSvgText::LineBreak linebreakStrictness = KoSvgText::LineBreak(this->textProperties().property(KoSvgTextProperties::LineBreakId).toInt());
    QVector<bool> collapseChars = KoCssTextUtils::collapseSpaces(&text, collapse);
    if (!lang.isEmpty()) {
        // Libunibreak currently only has support for strict, and even then only
        // for very specific cases.
        if (linebreakStrictness == KoSvgText::LineBreakStrict) {
            lang += "-strict";
        }
    }
    QVector<QPair<bool, bool>> justify;
    QVector<char> lineBreaks(text.size());
    QVector<char> graphemeBreaks(text.size());
    if (text.size() > 0) {
        // TODO: Figure out how to gracefully skip all the next steps when the text-size is 0.
        // can't currently remember if removing the associated outlines was all that is necessary.
        set_linebreaks_utf16(text.utf16(), static_cast<size_t>(text.size()), lang.toUtf8().data(), lineBreaks.data());
        set_graphemebreaks_utf16(text.utf16(), static_cast<size_t>(text.size()), lang.toUtf8().data(), graphemeBreaks.data());
        justify = KoCssTextUtils::justificationOpportunities(text, lang);
    }


    int globalIndex = 0;
    QVector<CharacterResult> result(text.size());
    // HACK ALERT!
    // Apparantly feeding a bidi algorithm a hardbreak makes it go 'ok, not doing any
    // bidi', which makes sense, Bidi is supossed to be done 'after' line breaking.
    // Without replacing hardbreaks with spaces, hardbreaks in rtl will break the bidi.
    for (int i = 0; i < text.size(); i++) {
        if (lineBreaks[i] == LINEBREAK_MUSTBREAK) {
            text[i] = QChar::Space;
        }
    }

    QMap<int, KoSvgText::TabSizeInfo> tabSizeInfo;

    // pass everything to a css-compatible text-layout algortihm.
    raqm_t_up layout(raqm_create());

    if (raqm_set_text_utf16(layout.data(), text.utf16(), static_cast<size_t>(text.size()))) {
        if (writingMode == KoSvgText::VerticalRL || writingMode == KoSvgText::VerticalLR) {
            raqm_set_par_direction(layout.data(), raqm_direction_t::RAQM_DIRECTION_TTB);
        } else if (direction == KoSvgText::DirectionRightToLeft) {
            raqm_set_par_direction(layout.data(), raqm_direction_t::RAQM_DIRECTION_RTL);
        } else {
            raqm_set_par_direction(layout.data(), raqm_direction_t::RAQM_DIRECTION_LTR);
        }

        int start = 0;
        Q_FOREACH (const KoSvgTextChunkShapeLayoutInterface::SubChunk &chunk, textChunks) {
            int length = chunk.text.size();
            KoSvgTextProperties properties = chunk.format.associatedShapeWrapper().shape()->textProperties();

            // In this section we retrieve the resolved transforms and
            // direction/anchoring that we can get from the subchunks.
            KoSvgText::TextAnchor anchor = KoSvgText::TextAnchor(properties.propertyOrDefault(KoSvgTextProperties::TextAnchorId).toInt());
            KoSvgText::Direction direction = KoSvgText::Direction(properties.propertyOrDefault(KoSvgTextProperties::DirectionId).toInt());
            KoSvgText::WordBreak wordBreakStrictness = KoSvgText::WordBreak(properties.propertyOrDefault(KoSvgTextProperties::WordBreakId).toInt());
            KoSvgText::HangingPunctuations hang =
                properties.propertyOrDefault(KoSvgTextProperties::HangingPunctuationId).value<KoSvgText::HangingPunctuations>();
            KoSvgText::TabSizeInfo tabInfo = properties.propertyOrDefault(KoSvgTextProperties::TabSizeId).value<KoSvgText::TabSizeInfo>();
            KoSvgText::AutoValue letterSpacing = properties.propertyOrDefault(KoSvgTextProperties::LetterSpacingId).value<KoSvgText::AutoValue>();
            KoSvgText::AutoValue wordSpacing = properties.propertyOrDefault(KoSvgTextProperties::WordSpacingId).value<KoSvgText::AutoValue>();
            KoSvgText::LineHeightInfo lineHeight = properties.propertyOrDefault(KoSvgTextProperties::LineHeightId).value<KoSvgText::LineHeightInfo>();
            bool overflowWrap = KoSvgText::OverflowWrap(properties.propertyOrDefault(KoSvgTextProperties::OverflowWrapId).toInt()) != KoSvgText::OverflowWrapNormal;

            if (!letterSpacing.isAuto) {
                tabInfo.extraSpacing += letterSpacing.customValue;
            }
            if (!wordSpacing.isAuto) {
                tabInfo.extraSpacing += wordSpacing.customValue;
            }

            for (int i = 0; i < length; i++) {
                CharacterResult cr = result[start + i];
                cr.anchor = anchor;
                cr.direction = direction;
                QPair<bool, bool> canJustify = justify.value(start + i, QPair<bool, bool>(false, false));
                cr.justifyBefore = canJustify.first;
                cr.justifyAfter = canJustify.second;
                cr.overflowWrap = overflowWrap;
                if (lineBreaks[start + i] == LINEBREAK_MUSTBREAK) {
                    cr.breakType = HardBreak;
                    cr.lineEnd = Collapse;
                    cr.lineStart = Collapse;
                } else if (lineBreaks[start + i] == LINEBREAK_ALLOWBREAK && wrap != KoSvgText::NoWrap) {
                    cr.breakType = SoftBreak;
                    if (KoCssTextUtils::collapseLastSpace(text.at(start + i), collapse)) {
                        cr.lineEnd = Collapse;
                        cr.lineStart = Collapse;
                    }
                }

                if ((wordBreakStrictness == KoSvgText::WordBreakBreakAll ||
                     linebreakStrictness == KoSvgText::LineBreakAnywhere)
                        && wrap != KoSvgText::NoWrap) {
                    if (graphemeBreaks[start + i] == GRAPHEMEBREAK_BREAK && cr.breakType == NoBreak) {
                        cr.breakType = SoftBreak;
                    }
                }
                if (cr.lineStart != Collapse && hang.testFlag(KoSvgText::HangFirst)) {
                    cr.lineStart = KoCssTextUtils::characterCanHang(text.at(start + i), KoSvgText::HangFirst) ? HangBehaviour : cr.lineEnd;
                }
                if (cr.lineEnd != Collapse) {
                    if (hang.testFlag(KoSvgText::HangLast)) {
                        cr.lineEnd = KoCssTextUtils::characterCanHang(text.at(start + i), KoSvgText::HangLast) ? HangBehaviour : cr.lineEnd;
                    }
                    if (hang.testFlag(KoSvgText::HangEnd)) {
                        LineEdgeBehaviour edge = hang.testFlag(KoSvgText::HangForce) ? ForceHang : ConditionallyHang;
                        cr.lineEnd = KoCssTextUtils::characterCanHang(text.at(start + i), KoSvgText::HangEnd) ? edge : cr.lineEnd;
                    }
                }
                if (text.at(start + i) == QChar::Tabulation) {
                    tabSizeInfo.insert(start + i, tabInfo);
                }

                if (chunk.firstTextInPath && i == 0) {
                    cr.anchored_chunk = true;
                }
                result[start + i] = cr;
            }

            QVector<int> lengths;
            QStringList fontFeatures = properties.fontFeaturesForText(start, length);

            qreal fontSize = properties.property(KoSvgTextProperties::FontSizeId).toReal();
            const QFont::Style style = QFont::Style(properties.propertyOrDefault(KoSvgTextProperties::FontStyleId).toInt());
            KoSvgText::AutoValue fontSizeAdjust = properties.propertyOrDefault(KoSvgTextProperties::FontSizeAdjustId).value<KoSvgText::AutoValue>();
            if (properties.hasProperty(KoSvgTextProperties::KraTextVersionId)) {
                fontSizeAdjust.isAuto = (properties.property(KoSvgTextProperties::KraTextVersionId).toInt() < 3);
            }
            const std::vector<FT_FaceUP> faces = KoFontRegistry::instance()->facesForCSSValues(
                properties.property(KoSvgTextProperties::FontFamiliesId).toStringList(),
                lengths,
                properties.fontAxisSettings(),
                chunk.text,
                static_cast<quint32>(finalRes),
                static_cast<quint32>(finalRes),
                fontSize,
                fontSizeAdjust.isAuto ? 1.0 : fontSizeAdjust.customValue,
                properties.propertyOrDefault(KoSvgTextProperties::FontWeightId).toInt(),
                properties.propertyOrDefault(KoSvgTextProperties::FontStretchId).toInt(),
                style != QFont::StyleNormal);
            if (properties.hasProperty(KoSvgTextProperties::TextLanguage)) {
                raqm_set_language(layout.data(),
                                  properties.property(KoSvgTextProperties::TextLanguage).toString().toUtf8(),
                                  static_cast<size_t>(start),
                                  static_cast<size_t>(length));
            }
            Q_FOREACH (const QString &feature, fontFeatures) {
                debugFlake << "adding feature" << feature;
                raqm_add_font_feature(layout.data(), feature.toUtf8(), feature.toUtf8().size());
            }

            if (!letterSpacing.isAuto) {
                raqm_set_letter_spacing_range(layout.data(),
                                              static_cast<int>(letterSpacing.customValue * ftFontUnit * scaleToPixel),
                                              static_cast<size_t>(start),
                                              static_cast<size_t>(length));
            }

            if (!wordSpacing.isAuto) {
                raqm_set_word_spacing_range(layout.data(),
                                            static_cast<int>(wordSpacing.customValue * ftFontUnit * scaleToPixel),
                                            static_cast<size_t>(start),
                                            static_cast<size_t>(length));
            }

            for (int i = 0; i < lengths.size(); i++) {
                length = lengths.at(i);
                FT_Int32 faceLoadFlags = loadFlags;
                const FT_FaceUP &face = faces.at(static_cast<size_t>(i));
                if (FT_HAS_COLOR(face)) {
                    faceLoadFlags |= FT_LOAD_COLOR;
                }
                if (!isHorizontal && FT_HAS_VERTICAL(face)) {
                    faceLoadFlags |= FT_LOAD_VERTICAL_LAYOUT;
                }
                if (start == 0) {
                    raqm_set_freetype_face(layout.data(), face.data());
                    raqm_set_freetype_load_flags(layout.data(), faceLoadFlags);
                }
                if (length > 0) {
                    raqm_set_freetype_face_range(layout.data(),
                                                 face.data(),
                                                 static_cast<size_t>(start),
                                                 static_cast<size_t>(length));
                    raqm_set_freetype_load_flags_range(layout.data(),
                                                       faceLoadFlags,
                                                       static_cast<size_t>(start),
                                                       static_cast<size_t>(length));
                }

                hb_font_t_up font(hb_ft_font_create_referenced(face.data()));
                hb_position_t ascender = 0;
                hb_position_t descender = 0;
                hb_position_t lineGap = 0;

                if (isHorizontal) {
                    /**
                     * There's 3 different definitions of the so-called vertical metrics, that is,
                     * the ascender and descender for horizontally laid out script. WinAsc & Desc,
                     * HHAE asc&desc, and OS/2... we need the last one, but harfbuzz doesn't return
                     * it unless there's a flag set in the font, which is missing in a lot of fonts
                     * that were from the transitional period, like Deja Vu Sans. Hence we need to get
                     * the OS/2 table and calculate the values manually (and fall back in various ways).
                     *
                     * https://www.w3.org/TR/css-inline-3/#ascent-descent
                     * https://www.w3.org/TR/CSS2/visudet.html#sTypoAscender
                     * https://wiki.inkscape.org/wiki/Text_Rendering_Notes#Ascent_and_Descent
                     *
                     * Related HB issue: https://github.com/harfbuzz/harfbuzz/issues/1920
                     */
                    TT_OS2 *os2Table = nullptr;
                    os2Table = (TT_OS2*)FT_Get_Sfnt_Table(face.data(), FT_SFNT_OS2);
                    if (os2Table) {
                        int yscale = face.data()->size->metrics.y_scale;

                        ascender = FT_MulFix(os2Table->sTypoAscender, yscale);
                        descender = FT_MulFix(os2Table->sTypoDescender, yscale);
                        lineGap = FT_MulFix(os2Table->sTypoLineGap, yscale);

                    } else {
                        if (!hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, &ascender)) {
                            ascender = face.data()->ascender;
                        }
                        if (!hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, &descender)) {
                            descender = face.data()->descender;
                        }
                        if (!hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP, &lineGap)) {
                            lineGap = face.data()->height - (ascender-descender);
                        }
                   }
                } else {
                    hb_font_extents_t fontExtends;
                    hb_font_get_extents_for_direction (font.data(), HB_DIRECTION_TTB, &fontExtends);
                    qreal height = fontExtends.ascender - fontExtends.descender;
                    if (!hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_VERTICAL_ASCENDER, &ascender)) {
                        ascender = height*0.5;
                    }
                    if (!hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_VERTICAL_DESCENDER, &descender)) {
                        descender = -(height*0.5);
                    }
                    if (!hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_VERTICAL_LINE_GAP, &lineGap)) {
                        lineGap = 0;
                    }
                }

                for (int j=start; j<start+length; j++) {
                    result[j].ascent = ascender;
                    result[j].descent = descender;
                    qreal leading = lineGap;

                    if (!lineHeight.isNormal) {
                        if (lineHeight.isNumber) {
                            leading = (fontSize*scaleToPixel*ftFontUnit)*lineHeight.value;
                            leading -= (ascender-descender);
                        } else {
                            QPointF val = ftTF.inverted().map(QPointF(lineHeight.value, lineHeight.value));
                            leading = isHorizontal? val.x(): val.y();
                            leading -= (ascender-descender);
                        }
                    }
                    result[j].halfLeading = leading*0.5;
                    result[j].fontStyle = style;
                    result[j].fontWeight = properties.propertyOrDefault(KoSvgTextProperties::FontWeightId).toInt();
                }

                start += length;
            }
        }
        debugFlake << "text-length:" << text.size();
    }
    // set very first character as anchored chunk.
    if (!result.empty()) {
        result[0].anchored_chunk = true;
    }

    if (raqm_layout(layout.data())) {
        debugFlake << "layout succeeded";
    }

    // 2. Set flags and assign initial positions
    // We also retreive a glyph path here.
    size_t count = 0;
    const raqm_glyph_t *glyphs = raqm_get_glyphs(layout.data(), &count);
    if (!glyphs) {
        return;
    }

    QPointF totalAdvanceFTFontCoordinates;
    QMap<int, int> logicalToVisual;


    KIS_ASSERT(count <= INT32_MAX);

    for (int i = 0; i < static_cast<int>(count); i++) {
        raqm_glyph_t currentGlyph = glyphs[i];
        KIS_ASSERT(currentGlyph.cluster <= INT32_MAX);
        const int cluster = static_cast<int>(currentGlyph.cluster);
        result[cluster].addressable = !collapseChars.at(cluster);
        if (!result[cluster].addressable) {
            continue;
        }
        CharacterResult charResult = result[cluster];

        FT_Int32 faceLoadFlags = loadFlags;
        if (!isHorizontal && FT_HAS_VERTICAL(currentGlyph.ftface)) {
            faceLoadFlags |= FT_LOAD_VERTICAL_LAYOUT;
        }
        if (FT_HAS_COLOR(currentGlyph.ftface)) {
            faceLoadFlags |= FT_LOAD_COLOR;
        }

        QPointF spaceAdvance;
        if (tabSizeInfo.contains(cluster)) {
            FT_Load_Glyph(currentGlyph.ftface, FT_Get_Char_Index(currentGlyph.ftface, ' '), faceLoadFlags);
            spaceAdvance = QPointF(currentGlyph.ftface->glyph->advance.x, currentGlyph.ftface->glyph->advance.y);
        }

        if (FT_Load_Glyph(currentGlyph.ftface, currentGlyph.index, faceLoadFlags) != 0) {
            continue;
        }

        debugFlake << "glyph" << i << "cluster" << cluster << currentGlyph.index << text.at(cluster).unicode();

        // Check whether we need to synthesize bold by emboldening the glyph:
        emboldenGlyphIfNeeded(currentGlyph, charResult);

        /// The matrix for Italic (oblique) synthesis of outline glyphs.
        QTransform glyphObliqueTf;
        /// The combined offset * italic * ftTf transform for outline glyphs.
        QTransform outlineGlyphTf;

        if (currentGlyph.ftface->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            outlineGlyphTf = QTransform::fromTranslate(currentGlyph.x_offset, currentGlyph.y_offset);

            // Check whether we need to synthesize italic by shearing the glyph:
            if (charResult.fontStyle != QFont::StyleNormal
                && !(currentGlyph.ftface->style_flags & FT_STYLE_FLAG_ITALIC)) {
                // CSS Fonts Module Level 4, 2.4. Font style: the font-style property:
                // For `oblique`, "lack of an <angle> represents 14deg".
                constexpr double SLANT_14DEG = 0.24932800284318069162403993780486;
                if (isHorizontal) {
                    glyphObliqueTf.shear(SLANT_14DEG, 0);
                } else {
                    // For vertical mode, CSSWG says:
                    // - Skew around the centre
                    // - Right-side down and left-side up
                    // https://github.com/w3c/csswg-drafts/issues/2869
                    glyphObliqueTf.shear(0, -SLANT_14DEG);
                }
                outlineGlyphTf *= glyphObliqueTf;
            }
            outlineGlyphTf *= ftTF;

            QPainterPath glyph = d->convertFromFreeTypeOutline(currentGlyph.ftface->glyph);
            glyph = outlineGlyphTf.map(glyph);

            if (!charResult.path.isEmpty()) {
                // this is for glyph clusters, unicode combining marks are always
                // added. we could have these as seperate paths, but there's no real
                // purpose, and the svg standard prefers 'ligatures' to be treated
                // as a single glyph. It simplifies things for us in any case.
                charResult.path.addPath(glyph.translated(charResult.advance));
            } else {
                charResult.path = glyph;
            }
        } else if (currentGlyph.ftface->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
            // TODO: Handle glyph clusters better...
            charResult.image = d->convertFromFreeTypeBitmap(currentGlyph.ftface->glyph);

            // Check whether we need to synthesize italic by shearing the glyph:
            if (charResult.fontStyle != QFont::StyleNormal
                && !(currentGlyph.ftface->style_flags & FT_STYLE_FLAG_ITALIC)) {
                // Since we are dealing with a bitmap glyph, we'll just use a nice
                // round floating point number.
                constexpr double SLANT_BITMAP = 0.25;
                QTransform bitmapTf;
                QPoint shearAt;
                if (isHorizontal) {
                    bitmapTf.shear(-SLANT_BITMAP, 0);
                    shearAt = QPoint(0, currentGlyph.ftface->glyph->bitmap_top);
                } else {
                    bitmapTf.shear(0, SLANT_BITMAP);
                    shearAt = QPoint(charResult.image.width() / 2, 0);
                }
                // We need to shear around the baseline, hence the translation.
                bitmapTf = QTransform::fromTranslate(-shearAt.x(), -shearAt.y()) * bitmapTf
                    * QTransform::fromTranslate(shearAt.x(), shearAt.y());
                charResult.image = std::move(charResult.image).transformed(
                    bitmapTf,
                    d->textRendering == OptimizeSpeed ? Qt::FastTransformation : Qt::SmoothTransformation);

                // This does the same as `QImage::trueMatrix` to get the image
                // offset after transforming.
                const QPoint offset = bitmapTf.mapRect(QRectF({0, 0}, charResult.image.size())).toAlignedRect().topLeft();
                currentGlyph.ftface->glyph->bitmap_left += offset.x();
                currentGlyph.ftface->glyph->bitmap_top -= offset.y();
            }
        } else {
            warnFlake << "Unsupported glyph format" << glyphFormatToStr(currentGlyph.ftface->glyph->format);
        }

        // Retreive CPAL/COLR V0 color layers, directly based off the sample
        // code in the freetype docs.
        FT_UInt layerGlyphIndex = 0;
        FT_UInt layerColorIndex = 0;
        FT_LayerIterator iterator;
        FT_Color *palette = nullptr;
        const unsigned short paletteIndex = 0;
        if (FT_Palette_Select(currentGlyph.ftface, paletteIndex, &palette) != 0) {
            palette = nullptr;
        }
        iterator.p = nullptr;
        bool haveLayers = FT_Get_Color_Glyph_Layer(currentGlyph.ftface,
                                                   currentGlyph.index,
                                                   &layerGlyphIndex,
                                                   &layerColorIndex,
                                                   &iterator);
        if (haveLayers && palette) {
            do {
                QBrush layerColor;
                bool isForeGroundColor = false;

                if (layerColorIndex == 0xFFFF) {
                    layerColor = Qt::black;
                    isForeGroundColor = true;
                } else {
                    FT_Color color = palette[layerColorIndex];
                    layerColor = QColor(color.red, color.green, color.blue, color.alpha);
                }
                FT_Load_Glyph(currentGlyph.ftface, layerGlyphIndex, faceLoadFlags);
                if (currentGlyph.ftface->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
                    // Check whether we need to synthesize bold by emboldening the glyph:
                    emboldenGlyphIfNeeded(currentGlyph, charResult);

                    QPainterPath p = d->convertFromFreeTypeOutline(currentGlyph.ftface->glyph);
                    p = outlineGlyphTf.map(p);
                    charResult.colorLayers.append(p);
                    charResult.colorLayerColors.append(layerColor);
                    charResult.replaceWithForeGroundColor.append(isForeGroundColor);
                } else {
                    warnFlake << "Unsupported glyph format" << glyphFormatToStr(currentGlyph.ftface->glyph->format) << "in glyph layers";
                }
            } while (FT_Get_Color_Glyph_Layer(currentGlyph.ftface,
                                              currentGlyph.index,
                                              &layerGlyphIndex,
                                              &layerColorIndex,
                                              &iterator));
        }

        charResult.visualIndex = i;
        logicalToVisual.insert(cluster, i);

        charResult.middle = false;
        QPointF advance(currentGlyph.x_advance, currentGlyph.y_advance);
        if (tabSizeInfo.contains(cluster)) {
            KoSvgText::TabSizeInfo tabSize = tabSizeInfo.value(cluster);
            qreal newAdvance = tabSize.value * ftFontUnit;
            if (tabSize.isNumber) {
                QPointF extraSpacing = isHorizontal ? QPointF(tabSize.extraSpacing * ftFontUnit, 0) : QPointF(0, tabSize.extraSpacing * ftFontUnit);
                advance = (spaceAdvance + extraSpacing) * tabSize.value;
            } else {
                advance = isHorizontal ? QPointF(newAdvance, advance.y()) : QPointF(advance.x(), newAdvance);
            }
            charResult.path = QPainterPath();
            charResult.image = QImage();
        }
        charResult.advance += ftTF.map(advance);

        bool usePixmap = !charResult.image.isNull() && charResult.path.isEmpty();

        QRectF bbox;



        if (usePixmap) {
            const int width = charResult.image.width();
            const int height = charResult.image.height();
            const int left = currentGlyph.ftface->glyph->bitmap_left;
            const int top = currentGlyph.ftface->glyph->bitmap_top - height;
            QRect bboxPixel(left, top, width, height);
            if (!isHorizontal) {
                bboxPixel.moveLeft(-(bboxPixel.width() / 2));
            }
            bbox = QRectF(bboxPixel.topLeft() * ftFontUnit, bboxPixel.size() * ftFontUnit);
        } else if (isHorizontal) {
            bbox = QRectF(0,
                          charResult.descent,
                          ftTF.inverted().map(charResult.advance).x(),
                          (charResult.ascent - charResult.descent));
            bbox = glyphObliqueTf.mapRect(bbox);
        } else {
            hb_font_t_up font(hb_ft_font_create_referenced(currentGlyph.ftface));
            bbox = QRectF(charResult.descent, 0, charResult.ascent - charResult.descent, ftTF.inverted().map(charResult.advance).y());
            bbox = glyphObliqueTf.mapRect(bbox);
        }
        charResult.boundingBox = ftTF.mapRect(bbox);
        charResult.halfLeading = ftTF.map(QPointF(charResult.halfLeading, charResult.halfLeading)).x();
        charResult.ascent = isHorizontal? charResult.boundingBox.top(): charResult.boundingBox.left();
        charResult.descent = isHorizontal? charResult.boundingBox.bottom(): charResult.boundingBox.right();

        if (!charResult.path.isEmpty()) {
            charResult.boundingBox |= charResult.path.boundingRect();
        }
        totalAdvanceFTFontCoordinates += advance;
        charResult.cssPosition = ftTF.map(totalAdvanceFTFontCoordinates) - charResult.advance;

        result[cluster] = charResult;
    }

    // fix it so that characters that are in the 'middle' due to either being
    // surrogates or part of a ligature, are marked as such.
    // Also ensure that anchored chunks get set to the first addressable non-middle characters.
    int firstCluster = 0;
    bool setAnchoredChunk = false;
    for (int i = 0; i < result.size(); i++) {
        if (result[i].addressable && result.at(i).visualIndex != -1) {
            firstCluster = i;
            if (setAnchoredChunk) {
                result[i].anchored_chunk = true;
                setAnchoredChunk = false;

            }
        } else {
            if (result[firstCluster].breakType != HardBreak) {
                result[firstCluster].breakType = result.at(i).breakType;
            }
            if (result[firstCluster].lineStart == NoChange) {
                result[firstCluster].lineStart = result.at(i).lineStart;
            }
            if (result[firstCluster].lineEnd == NoChange) {
                result[firstCluster].lineEnd = result.at(i).lineEnd;
            }
            if (result[i].anchored_chunk) {
                setAnchoredChunk = true;
            }
            result[i].middle = true;
            result[i].addressable = false;
        }
    }
    debugFlake << "Glyphs retreived";

    // 3. Resolve character positioning.
    QVector<KoSvgText::CharTransformation> resolvedTransforms(text.size());
    if (!resolvedTransforms.empty()) {
        // Ensure the first entry defaults to 0.0 for x and y, otherwise textAnchoring
        // will not work for text that has been bidi-reordered.
        resolvedTransforms[0].xPos = 0.0;
        resolvedTransforms[0].yPos = 0.0;
    }
    globalIndex = 0;
    d->resolveTransforms(this, globalIndex, isHorizontal, false, resolvedTransforms, collapseChars);

    // Handle linebreaking.
    QPointF startPos = resolvedTransforms[0].absolutePos();


    if (!d->shapesInside.isEmpty()) {
        QList<QPainterPath> shapes = d->getShapes(d->shapesInside, d->shapesSubtract, this->textProperties());
        d->lineBoxes = d->flowTextInShapes(this->textProperties(), logicalToVisual, result, shapes);
    } else {
        d->lineBoxes = d->breakLines(this->textProperties(), logicalToVisual, result, startPos);
    }
    // Handle baseline alignment.
    globalIndex = 0;
    d->computeFontMetrics(this, QMap<int, int>(), 0, QPointF(), QPointF(), result, d->lineBoxes, globalIndex, finalRes, isHorizontal);

    if (inlineSize.isAuto && d->shapesInside.isEmpty()) {
        debugFlake << "Starting with SVG 1.1 specific portion";
        debugFlake << "4. Adjust positions: dx, dy";
        // 4. Adjust positions: dx, dy
        QPointF shift = QPointF();

        for (int i = 0; i < result.size(); i++) {
            if (result.at(i).addressable) {
                KoSvgText::CharTransformation transform = resolvedTransforms[i];
                if (transform.hasRelativeOffset()) {
                    shift += transform.relativeOffset();
                }
                CharacterResult charResult = result[i];
                if (transform.rotate) {
                    charResult.rotate = *transform.rotate;
                }
                charResult.finalPosition = charResult.cssPosition + shift;
                if (transform.startsNewChunk()) {
                    charResult.anchored_chunk = true;
                }
                result[i] = charResult;
            }
        }

        // 5. Apply ‘textLength’ attribute
        debugFlake << "5. Apply ‘textLength’ attribute";
        globalIndex = 0;
        int resolved = 0;
        d->applyTextLength(this, result, globalIndex, resolved, isHorizontal);

        // 6. Adjust positions: x, y
        debugFlake << "6. Adjust positions: x, y";
        // https://github.com/w3c/svgwg/issues/617
        shift = QPointF();
        for (int i = 0; i < result.size(); i++) {
            if (result.at(i).addressable) {
                KoSvgText::CharTransformation transform = resolvedTransforms[i];
                CharacterResult charResult = result[i];
                if (transform.xPos) {
                    const qreal delta = transform.dxPos ? *transform.dxPos : 0.0;
                    shift.setX(*transform.xPos + (delta - charResult.finalPosition.x()));
                }
                if (transform.yPos) {
                    const qreal delta = transform.dyPos ? *transform.dyPos : 0.0;
                    shift.setY(*transform.yPos + (delta - charResult.finalPosition.y()));
                }
                charResult.finalPosition += shift;

                result[i] = charResult;
            }
        }

        // 7. Apply anchoring
        debugFlake << "7. Apply anchoring";
        d->applyAnchoring(result, isHorizontal);

        // Computing the textDecorations needs to happen before applying the
        // textPath to the results, as we need the unapplied result vector for
        // positioning.
        debugFlake << "Now Computing text-decorations";
        globalIndex = 0;
        d->computeTextDecorations(this,
                                  result,
                                  logicalToVisual,
                                  minimumDecorationThickness,
                                  nullptr,
                                  0.0,
                                  false,
                                  globalIndex,
                                  isHorizontal,
                                  direction == KoSvgText::DirectionLeftToRight,
                                  false);

        // 8. Position on path

        debugFlake << "8. Position on path";
        d->applyTextPath(this, result, isHorizontal);
    } else {
        globalIndex = 0;
        debugFlake << "Computing text-decorationsfor inline-size";
        d->computeTextDecorations(this,
                                  result,
                                  logicalToVisual,
                                  minimumDecorationThickness,
                                  nullptr,
                                  0.0,
                                  false,
                                  globalIndex,
                                  isHorizontal,
                                  direction == KoSvgText::DirectionLeftToRight,
                                  true);
    }

    // 9. return result.
    debugFlake << "9. return result.";
    d->result = result;
    globalIndex = 0;
    Q_FOREACH (const KoSvgTextChunkShapeLayoutInterface::SubChunk &chunk, textChunks) {
        KoSvgText::AssociatedShapeWrapper wrapper = chunk.format.associatedShapeWrapper();
        const int j = chunk.text.size();
        for (int i = globalIndex; i < globalIndex + j; i++) {
            if (result.at(i).addressable && !result.at(i).hidden) {
                const QTransform tf = result.at(i).finalTransform();
                wrapper.addCharacterRect(tf.mapRect(result.at(i).boundingBox));
            }
        }
        globalIndex += j;
    }
}

void KoSvgTextShape::Private::clearAssociatedOutlines(const KoShape *rootShape)
{
    const KoSvgTextChunkShape *chunkShape = dynamic_cast<const KoSvgTextChunkShape *>(rootShape);
    KIS_SAFE_ASSERT_RECOVER_RETURN(chunkShape);

    chunkShape->layoutInterface()->clearAssociatedOutline();

    Q_FOREACH (KoShape *child, chunkShape->shapes()) {
        clearAssociatedOutlines(child);
    }
}

void KoSvgTextShape::Private::resolveTransforms(const KoShape *rootShape, int &currentIndex, bool isHorizontal, bool textInPath, QVector<KoSvgText::CharTransformation> &resolved, QVector<bool> collapsedChars) {
    const KoSvgTextChunkShape *chunkShape = dynamic_cast<const KoSvgTextChunkShape *>(rootShape);
    KIS_SAFE_ASSERT_RECOVER_RETURN(chunkShape);

    QVector<KoSvgText::CharTransformation> local = chunkShape->layoutInterface()->localCharTransformations();

    int i = 0;

    int index = currentIndex;
    int j = index + chunkShape->layoutInterface()->numChars(true);

    if (chunkShape->layoutInterface()->textPath()) {
        textInPath = true;
    } else {
        for (int k = index; k < j; k++ ) {
            if (collapsedChars[k]) {
                continue;
            }

            if (i < local.size()) {
                KoSvgText::CharTransformation newTransform = local.at(i);
                newTransform.mergeInParentTransformation(resolved[k]);
                resolved[k] = newTransform;
                i += 1;
            } else if (k > 0) {
                if (resolved[k - 1].rotate) {
                    resolved[k].rotate = resolved[k - 1].rotate;
                }
            }
        }
    }

    Q_FOREACH (KoShape *child, chunkShape->shapes()) {
        resolveTransforms(child, currentIndex, isHorizontal, textInPath, resolved, collapsedChars);

    }

    if (chunkShape->layoutInterface()->textPath()) {
        bool first = true;
        for (int k = index; k < j; k++ ) {

            if (collapsedChars[k]) {
                continue;
            }

            //  Also unset the first transform on a textPath to avoid breakage with rtl text.
            if (first) {
                if (isHorizontal) {
                    resolved[k].xPos = 0.0;
                } else {
                    resolved[k].yPos = 0.0;
                }
                first = false;
            }
            // x and y attributes are officially 'ignored' for text on path, though the algorithm
            // suggests this is only if a child of a path... In reality, not resetting this will
            // break text-on-path with rtl.
            if (isHorizontal) {
                resolved[k].yPos.reset();
            } else {
                resolved[k].xPos.reset();
            }
        }
    }

    currentIndex = j;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
QPainterPath KoSvgTextShape::Private::convertFromFreeTypeOutline(FT_GlyphSlotRec *glyphSlot)
{
    QPointF cp = QPointF();
    // convert the outline to a painter path
    // This is taken from qfontengine_ft.cpp.
    QPainterPath glyph;
    glyph.setFillRule(Qt::WindingFill);
    int i = 0;
    for (int j = 0; j < glyphSlot->outline.n_contours; ++j) {
        int last_point = glyphSlot->outline.contours[j];
        // qDebug() << "contour:" << i << "to" << last_point;
        QPointF start = QPointF(glyphSlot->outline.points[i].x, glyphSlot->outline.points[i].y);
        if (!(glyphSlot->outline.tags[i] & 1)) { // start point is not on curve:
            if (!(glyphSlot->outline.tags[last_point] & 1)) { // end point is not on curve:
                // qDebug() << "  start and end point are not on curve";
                start = (QPointF(glyphSlot->outline.points[last_point].x, glyphSlot->outline.points[last_point].y) + start) / 2.0;
            } else {
                // qDebug() << "  end point is on curve, start is not";
                start = QPointF(glyphSlot->outline.points[last_point].x, glyphSlot->outline.points[last_point].y);
            }
            --i; // to use original start point as control point below
        }
        start += cp;
        // qDebug() << "  start at" << start;
        glyph.moveTo(start);
        std::array<QPointF, 4> curve;
        curve[0] = start;
        size_t n = 1;
        while (i < last_point) {
            ++i;
            curve.at(n) = cp + QPointF(glyphSlot->outline.points[i].x, glyphSlot->outline.points[i].y);
            // qDebug() << "    " << i << c[n] << "tag =" <<
            // (int)g->outline.tags[i]
            //                    << ": on curve =" << (bool)(g->outline.tags[i]
            //                    & 1);
            ++n;
            switch (glyphSlot->outline.tags[i] & 3) {
            case 2:
                // cubic bezier element
                if (n < 4)
                    continue;
                curve[3] = (curve[3] + curve[2]) / 2;
                --i;
                break;
            case 0:
                // quadratic bezier element
                if (n < 3)
                    continue;
                curve[3] = (curve[1] + curve[2]) / 2;
                curve[2] = (2 * curve[1] + curve[3]) / 3;
                curve[1] = (2 * curve[1] + curve[0]) / 3;
                --i;
                break;
            case 1:
            case 3:
                if (n == 2) {
                    // qDebug() << "  lineTo" << c[1];
                    glyph.lineTo(curve[1]);
                    curve[0] = curve[1];
                    n = 1;
                    continue;
                } else if (n == 3) {
                    curve[3] = curve[2];
                    curve[2] = (2 * curve[1] + curve[3]) / 3;
                    curve[1] = (2 * curve[1] + curve[0]) / 3;
                }
                break;
            }
            // qDebug() << "  cubicTo" << c[1] << c[2] << c[3];
            glyph.cubicTo(curve[1], curve[2], curve[3]);
            curve[0] = curve[3];
            n = 1;
        }
        if (n == 1) {
            // qDebug() << "  closeSubpath";
            glyph.closeSubpath();
        } else {
            curve[3] = start;
            if (n == 2) {
                curve[2] = (2 * curve[1] + curve[3]) / 3;
                curve[1] = (2 * curve[1] + curve[0]) / 3;
            }
            // qDebug() << "  close cubicTo" << c[1] << c[2] << c[3];
            glyph.cubicTo(curve[1], curve[2], curve[3]);
        }
        ++i;
    }
    return glyph;
}

QImage KoSvgTextShape::Private::convertFromFreeTypeBitmap(FT_GlyphSlotRec *glyphSlot)
{
    KIS_ASSERT(glyphSlot->bitmap.width <= INT32_MAX);
    KIS_ASSERT(glyphSlot->bitmap.rows <= INT32_MAX);
    QImage img;
    const int height = static_cast<int>(glyphSlot->bitmap.rows);
    const QSize size(static_cast<int>(glyphSlot->bitmap.width), height);

    if (glyphSlot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        img = QImage(size, QImage::Format_Mono);
        uchar *src = glyphSlot->bitmap.buffer;
        KIS_ASSERT(glyphSlot->bitmap.pitch >= 0);
        for (int y = 0; y < height; y++) {
            memcpy(img.scanLine(y), src, static_cast<size_t>(glyphSlot->bitmap.pitch));
            src += glyphSlot->bitmap.pitch;
        }
    } else if (glyphSlot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        img = QImage(size, QImage::Format_Grayscale8);
        uchar *src = glyphSlot->bitmap.buffer;
        KIS_ASSERT(glyphSlot->bitmap.pitch >= 0);
        for (int y = 0; y < height; y++) {
            memcpy(img.scanLine(y), src, static_cast<size_t>(glyphSlot->bitmap.pitch));
            src += glyphSlot->bitmap.pitch;
        }
    } else if (glyphSlot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        img = QImage(size, QImage::Format_ARGB32_Premultiplied);
        const uint8_t *src = glyphSlot->bitmap.buffer;
        for (int y = 0; y < height; y++) {
            auto *argb = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (unsigned int x = 0; x < glyphSlot->bitmap.width; x++) {
                argb[x] = qRgba(src[2], src[1], src[0], src[3]);
                src += 4;
            }
        }
    }

    return img;
}


QList<QPainterPath> KoSvgTextShape::Private::getShapes(QList<KoShape *> shapesInside,
                                                       QList<KoShape *> shapesSubtract,
                                                       const KoSvgTextProperties &properties) {

    // the boost polygon method requires (and gives best result) on a inter-based polygon,
    // so we need to scale up. The scale selected here is the size freetype coordinates give to a single pixel.
    qreal scale = 64.0;
    QTransform precisionTF = QTransform::fromScale(scale, scale);

    qreal shapePadding = scale * properties.propertyOrDefault(KoSvgTextProperties::ShapePaddingId).toReal();
    qreal shapeMargin = scale * properties.propertyOrDefault(KoSvgTextProperties::ShapeMarginId).toReal();

    QPainterPath subtract;
    Q_FOREACH(const KoShape *shape, shapesSubtract) {
        const KoPathShape *path = dynamic_cast<const KoPathShape*>(shape);
        if (path) {
            QPainterPath p = path->transformation().map(path->outline());
            p.setFillRule(path->fillRule());
            // grow each polygon here with the shape margin size.
            if (shapeMargin > 0) {
                QList<QPolygon> subpathPolygons;
                Q_FOREACH(QPolygonF subPath, p.toSubpathPolygons()) {
                    subpathPolygons.append(precisionTF.map(subPath).toPolygon());
                }
                subpathPolygons = KoPolygonUtils::offsetPolygons(subpathPolygons, shapeMargin);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
                p.clear();
#else
                p = QPainterPath();
#endif

                Q_FOREACH (const QPolygon poly, subpathPolygons) {
                    p.addPolygon(poly);
                }
            } else {
                p = precisionTF.map(p);
            }
            subtract.addPath(p);
        }
    }

    QList<QPainterPath> shapes;
    Q_FOREACH(const KoShape *shape, shapesInside) {
        const KoPathShape *path = dynamic_cast<const KoPathShape*>(shape);
        if (path) {
            QPainterPath p = path->transformation().map(path->outline());
            p.setFillRule(path->fillRule());
            QPainterPath p2;
            p2.setFillRule(path->fillRule());

            QList<QPolygon> subpathPolygons;
            Q_FOREACH(QPolygonF subPath, p.toSubpathPolygons()) {
                subpathPolygons.append(precisionTF.map(subPath).toPolygon());
            }
            subpathPolygons = KoPolygonUtils::offsetPolygons(subpathPolygons, -shapePadding);

            for (int i=0; i < subpathPolygons.size(); i++) {
                QPolygonF subpathPoly = subpathPolygons.at(i);
                Q_FOREACH(QPolygonF subtractPoly, subtract.toSubpathPolygons()) {
                    if (subpathPoly.intersects(subtractPoly)) {
                        subpathPoly = subpathPoly.subtracted(subtractPoly);
                    }
                }
                p2.addPolygon(subpathPoly);
            }
            shapes.append(precisionTF.inverted().map(p2));
        }
    }
    return shapes;
}

/**
 * @brief addWordToLine
 * Small function used in break lines to quickly add a 'word' to the current
 * line. Returns the last added index.
 */
void addWordToLine(QVector<CharacterResult> &result,
                   QPointF &currentPos,
                   QVector<int> &wordIndices,
                   LineBox &currentLine,
                   bool ltr)
{
    QPointF lineAdvance = currentPos;

    LineChunk currentChunk  = currentLine.chunk();

    Q_FOREACH (const int j, wordIndices) {
        CharacterResult cr = result.at(j);
        if (currentChunk.boundingBox.isEmpty() && j == wordIndices.first()) {
            if (result.at(j).lineStart == Collapse) {
                result[j].addressable = false;
                result[j].hidden = true;
                continue;
            }
            cr.anchored_chunk = true;
            if (result.at(j).lineStart == HangBehaviour && currentLine.firstLine) {
                if (ltr) {
                    currentPos -= cr.advance;
                } else {
                    currentPos += cr.advance;
                }
                cr.isHanging = true;
            }
        }
        cr.cssPosition = currentPos;
        currentPos += cr.advance;
        if (currentLine.firstLine) {
        }
        lineAdvance = currentPos;

        result[j] = cr;
        currentChunk.boundingBox |= cr.boundingBox.translated(cr.cssPosition);

        currentLine.actualLineTop = qMax(fabs(cr.ascent-cr.halfLeading), currentLine.actualLineTop);
        currentLine.actualLineBottom = qMax(fabs(cr.descent+cr.halfLeading), currentLine.actualLineBottom);
    }
    currentPos = lineAdvance;
    currentChunk.chunkIndices += wordIndices;
    currentLine.setCurrentChunk(currentChunk);
    wordIndices.clear();
}

/**
 * This offsets the last line by it's ascent, and then returns the last line's
 * descent.
 */
QPointF lineHeightOffset(KoSvgText::WritingMode writingMode,
                         QVector<CharacterResult> &result,
                         LineBox &currentLine,
                         bool firstLine)
{
    QPointF lineTop;
    QPointF lineBottom;
    QPointF correctionOffset;

    if (currentLine.chunks.isEmpty()) {
        return QPointF();
    } else if (currentLine.chunks.size() == 1 && currentLine.actualLineTop == 0 &&
               currentLine.actualLineBottom == 0){
        /**
         * When the line is empty, but caused by a hardbreak, we will need to use that hardbreak
         * to space the line. This can only be done at this point as it would otherwise need to use
         * visible characters.
         */
        QVector<int> chunkIndices = currentLine.chunks[0].chunkIndices;
        if (chunkIndices.size() > 0) {
            CharacterResult cr = result[chunkIndices.first()];
            currentLine.actualLineTop = qMax(fabs(cr.ascent-cr.halfLeading), currentLine.actualLineTop);
            currentLine.actualLineBottom = qMax(fabs(cr.descent+cr.halfLeading), currentLine.actualLineBottom);
        }
    }

    qreal expectedLineTop = qMax(currentLine.expectedLineTop, currentLine.actualLineTop);
    if (writingMode == KoSvgText::HorizontalTB) {
        lineTop = QPointF(0, currentLine.actualLineTop);
        lineBottom = QPointF(0, currentLine.actualLineBottom);
        correctionOffset = QPointF(0, expectedLineTop);
        currentLine.baselineTop = -lineTop;
        currentLine.baselineBottom = lineBottom;
    } else if (writingMode == KoSvgText::VerticalLR) {
        lineTop = QPointF(currentLine.actualLineTop, 0);
        lineBottom = QPointF(currentLine.actualLineBottom, 0);
        correctionOffset = QPointF(expectedLineTop, 0);
        // Note: while Vertical LR goes left-to-right in its lines, its lines themselves are
        // oriented with the top pointed in the positive x directi´on.
        currentLine.baselineTop = lineTop;
        currentLine.baselineBottom = -lineBottom;
    } else {
        lineTop = QPointF(-currentLine.actualLineTop, 0);
        lineBottom = QPointF(-currentLine.actualLineBottom, 0);
        correctionOffset = QPointF(-expectedLineTop, 0);
        currentLine.baselineTop = lineTop;
        currentLine.baselineBottom = -lineBottom;
    }
    bool returnDescent = firstLine;
    QPointF offset = lineTop + lineBottom;

    correctionOffset -= lineTop;
    if (!returnDescent) {
        Q_FOREACH(LineChunk chunk, currentLine.chunks) {
            Q_FOREACH (int j, chunk.chunkIndices) {
                result[j].cssPosition += lineTop;
                result[j].finalPosition = result.at(j).cssPosition;
            }
            chunk.length.translate(lineTop);
            chunk.boundingBox.translate(lineTop);
        }
    } else {
        offset = lineBottom - correctionOffset;
        Q_FOREACH(LineChunk chunk, currentLine.chunks) {
            Q_FOREACH (int j, chunk.chunkIndices) {
                result[j].cssPosition -= correctionOffset;
                result[j].finalPosition = result.at(j).cssPosition;
            }
            chunk.length.translate(-correctionOffset);
            chunk.boundingBox.translate(-correctionOffset);
        }
    }
    return offset;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void handleCollapseAndHang(QVector<CharacterResult> &result,
                           LineChunk chunk,
                           bool inlineSize,
                           bool ltr,
                           bool atEnd)
{
    QVector<int> lineIndices = chunk.chunkIndices;
    QPointF endPos = chunk.length.p2();

    if (!lineIndices.isEmpty()) {
        QVectorIterator<int> it(lineIndices);
        it.toBack();
        while (it.hasPrevious()) {
            int lastIndex = it.previous();
            if (result.at(lastIndex).lineEnd == Collapse) {
                result[lastIndex].addressable = false;
                result[lastIndex].hidden = true;
            } else if (result.at(lastIndex).lineEnd == ForceHang && inlineSize) {
                QPointF pos = endPos;
                if (!ltr) {
                    pos -= result.at(lastIndex).advance;
                }
                result[lastIndex].cssPosition = pos;
                result[lastIndex].finalPosition = pos;
                result[lastIndex].isHanging = true;
            } else if (result.at(lastIndex).lineEnd == HangBehaviour && inlineSize && atEnd) {
                QPointF pos = endPos;
                if (!ltr) {
                    pos -= result.at(lastIndex).advance;
                }
                result[lastIndex].cssPosition = pos;
                result[lastIndex].finalPosition = pos;
                result[lastIndex].isHanging = true;
            }
            if (result.at(lastIndex).lineEnd != Collapse) {
                break;
            }
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void applyInlineSizeAnchoring(QVector<CharacterResult> &result,
                              LineChunk chunk,
                              KoSvgText::TextAnchor anchor,
                              QPointF anchorPoint,
                              bool ltr,
                              bool isHorizontal,
                              QPointF textIndent)
{
    QVector<int> lineIndices = chunk.chunkIndices;
    QPointF startPos = anchorPoint;
    qreal shift = isHorizontal ? startPos.x() : startPos.y();

    qreal a = 0;
    qreal b = 0;

    QPointF aStartPos = chunk.length.p1();
    const QPointF inlineWidth = aStartPos - chunk.length.p2();
    QPointF aEndPos = aStartPos - inlineWidth;

    Q_FOREACH (int i, lineIndices) {
        if (!result.at(i).addressable || result.at(i).isHanging) {
            continue;
        }
        const qreal pos = isHorizontal ? result.at(i).finalPosition.x() : result.at(i).finalPosition.y();
        const qreal advance = isHorizontal ? result.at(i).advance.x() : result.at(i).advance.y();

        if (i == lineIndices.first()) {
            a = qMin(pos, pos + advance);
            b = qMax(pos, pos + advance);
        } else {
            a = qMin(a, qMin(pos, pos + advance));
            b = qMax(b, qMax(pos, pos + advance));
        }
    }

    if (anchor == KoSvgText::AnchorStart) {
        const qreal indent = isHorizontal ? textIndent.x() : textIndent.y();
        if (ltr) {
            a -= indent;
        } else {
            b += indent;
        }
    }

    if (anchor == KoSvgText::AnchorEnd) {
        aEndPos = aStartPos;
        aStartPos = aStartPos + inlineWidth;
    }

    if ((anchor == KoSvgText::AnchorStart && ltr) || (anchor == KoSvgText::AnchorEnd && !ltr)) {
        shift -= a;

    } else if ((anchor == KoSvgText::AnchorEnd && ltr) || (anchor == KoSvgText::AnchorStart && !ltr)) {
        shift -= b;

    } else {
        aEndPos = (startPos + aEndPos) * 0.5;
        aStartPos = startPos - aEndPos;
        shift -= ((a + b) * 0.5);
    }

    QPointF shiftP = isHorizontal ? QPointF(shift, 0) : QPointF(0, shift);
    Q_FOREACH (int j, lineIndices) {
        if (!result.at(j).isHanging) {
            result[j].cssPosition += shiftP;
            result[j].finalPosition = result.at(j).cssPosition;
        } else if (result.at(j).anchored_chunk) {
            QPointF shift = aStartPos;
            shift = ltr ? shift - result.at(j).advance : shift;
            result[j].cssPosition = shift;
            result[j].finalPosition = result.at(j).cssPosition;
        } else if (result.at(j).lineEnd != NoChange) {
            QPointF shift = aEndPos;
            shift = ltr ? shift : shift - result.at(j).advance;
            result[j].cssPosition = shift;
            result[j].finalPosition = result.at(j).cssPosition;
        }
    }
}

/// Finalizing the line consists of several steps, like hang/collapse, anchoring
/// into place and offsetting correctly. This can happen several times during a
/// linebreak, hence this convenience function to handle this.
void finalizeLine(QVector<CharacterResult> &result,
                  QPointF &currentPos,
                  LineBox &currentLine,
                  QPointF &lineOffset,
                  KoSvgText::TextAnchor anchor,
                  KoSvgText::WritingMode writingMode,
                  bool ltr,
                  bool inlineSize,
                  bool textInShape)
{
    bool isHorizontal = writingMode == KoSvgText::HorizontalTB;

    bool firstLine = textInShape? true: currentLine.firstLine;

    Q_FOREACH (LineChunk currentChunk, currentLine.chunks) {
        QMap<int, int> visualToLogical;
        Q_FOREACH (int j, currentChunk.chunkIndices) {
            visualToLogical.insert(result.at(j).visualIndex, j);
        }
        currentPos = lineOffset;

        handleCollapseAndHang(result, currentChunk, inlineSize, ltr, currentLine.lastLine);

        QPointF justifyOffset;
        if (currentLine.justifyLine) {
            int justificationCount = 0;
            Q_FOREACH (int j, visualToLogical.values()) {
                if (!result.at(j).addressable || result.at(j).isHanging) {
                    continue;
                }
                if (result.at(j).justifyBefore && j!= visualToLogical.values().first()) {
                    justificationCount += 1;
                }
                if (result.at(j).justifyAfter && j!= visualToLogical.values().last()) {
                    justificationCount += 1;
                }
            }

            if (justificationCount > 0) {
                if (isHorizontal) {
                    qreal val = currentChunk.length.length()-currentChunk.boundingBox.width();
                    val = val / justificationCount;
                    justifyOffset = QPointF(val, 0);
                } else {
                    qreal val = currentChunk.length.length()-currentChunk.boundingBox.height();
                    val = val / justificationCount;
                    justifyOffset = QPointF(0, val);
                }
            }
        }

        Q_FOREACH (const int j, visualToLogical.values()) {
            if (!result.at(j).addressable || result.at(j).isHanging) {
                continue;
            }
            if (result.at(j).justifyBefore) {
                currentPos += justifyOffset;
            }
            result[j].cssPosition = currentPos;
            result[j].finalPosition = currentPos;
            currentPos = currentPos + result.at(j).advance;
            if (result.at(j).justifyAfter) {
                currentPos += justifyOffset;
            }
        }

        if (inlineSize) {
            QPointF anchorPoint = currentChunk.length.p1();
            if (textInShape) {
                if (anchor == KoSvgText::AnchorMiddle) {
                    anchorPoint = currentChunk.length.center();
                } else if (anchor == KoSvgText::AnchorEnd) {
                    anchorPoint = currentChunk.length.p2();
                }
            }
            applyInlineSizeAnchoring(result, currentChunk, anchor, anchorPoint, ltr, isHorizontal, currentLine.textIndent);
        }
    }
    lineOffset += lineHeightOffset(writingMode, result, currentLine, firstLine);
    currentPos = lineOffset;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
QVector<LineBox> KoSvgTextShape::Private::breakLines(const KoSvgTextProperties &properties,
                                         const QMap<int, int> &logicalToVisual,
                                         QVector<CharacterResult> &result,
                                         QPointF startPos)
{
    KoSvgText::WritingMode writingMode = KoSvgText::WritingMode(properties.propertyOrDefault(KoSvgTextProperties::WritingModeId).toInt());
    KoSvgText::Direction direction = KoSvgText::Direction(properties.propertyOrDefault(KoSvgTextProperties::DirectionId).toInt());
    KoSvgText::AutoValue inlineSize = properties.propertyOrDefault(KoSvgTextProperties::InlineSizeId).value<KoSvgText::AutoValue>();
    KoSvgText::TextAnchor anchor = KoSvgText::TextAnchor(properties.propertyOrDefault(KoSvgTextProperties::TextAnchorId).toInt());

    bool ltr = direction == KoSvgText::DirectionLeftToRight;
    bool isHorizontal = writingMode == KoSvgText::HorizontalTB;

    QVector<LineBox> lineBoxes;

    QPointF endPos; ///< Used for hanging glyphs at the end of a line.

    KoSvgText::TextIndentInfo textIndentInfo = properties.propertyOrDefault(KoSvgTextProperties::TextIndentId).value<KoSvgText::TextIndentInfo>();
    QPointF textIndent;
    if (!inlineSize.isAuto) {
        qreal textIdentValue = textIndentInfo.value;
        if (textIndentInfo.isPercentage) {
            textIndent *= inlineSize.customValue;
        }
        if (isHorizontal) {
            textIndent = QPointF(textIdentValue, 0);
            endPos = ltr ? QPointF(startPos.x() + inlineSize.customValue, 0) : QPointF(startPos.x() - inlineSize.customValue, 0);
        } else {
            textIndent = QPointF(0, textIdentValue);
            endPos = ltr ? QPointF(0, startPos.y() + inlineSize.customValue) : QPointF(0, startPos.y() - inlineSize.customValue);
        }
    }
    LineBox currentLine(startPos, endPos);
    currentLine.firstLine = true;

    QVector<int> wordIndices; ///< 'word' in this case meaning characters
                              ///< inbetween softbreaks.
    QPointF wordAdvance; ///< Approximated advance of the current wordindices.

    QPointF currentPos = startPos; ///< Current position with advances of each character.
    if (!textIndentInfo.hanging && !inlineSize.isAuto) {
        currentLine.textIndent = textIndent;
        currentPos += currentLine.textIndent;
    }
    QPointF lineOffset = startPos; ///< Current line offset.

    QVector<int> lineIndices; ///< Indices of characters in line.

    QListIterator<int> it(logicalToVisual.keys());
    while (it.hasNext()) {
        int index = it.next();
        CharacterResult charResult = result.at(index);
        if (!charResult.addressable) {
            continue;
        }
        bool softBreak = false; ///< Whether to do a softbreak;
        bool doNotCountAdvance =
            ((charResult.lineEnd != NoChange)
             && !(currentLine.isEmpty() && wordIndices.isEmpty()));
        if (!doNotCountAdvance) {
            if (wordIndices.isEmpty()) {
                wordAdvance = charResult.advance;
            } else {
                wordAdvance += charResult.advance;
            }
        }
        wordIndices.append(index);
        currentLine.lastLine = !it.hasNext();

        if (charResult.breakType != NoBreak || currentLine.lastLine) {
            qreal lineLength = isHorizontal ? (currentPos - startPos + wordAdvance).x()
                                            : (currentPos - startPos + wordAdvance).y();
            if (!inlineSize.isAuto) {
                // Sometimes glyphs are a fraction larger than you'd expect, but
                // not enough to really break the line, so the following is a
                // bit more stable than a simple compare.
                if (qRound((abs(lineLength) - inlineSize.customValue)) > 0) {
                    softBreak = true;
                } else {
                    addWordToLine(result, currentPos, wordIndices, currentLine, ltr);
                }
            } else {
                addWordToLine(result, currentPos, wordIndices, currentLine, ltr);
            }
        }

        if (softBreak) {
            bool firstLine = currentLine.firstLine;
            if (!currentLine.isEmpty()) {
                finalizeLine(result,
                             currentPos,
                             currentLine,
                             lineOffset,
                             anchor,
                             writingMode,
                             ltr,
                             !inlineSize.isAuto,
                             false);
                lineBoxes.append(currentLine);
                currentLine.clearAndAdjust(isHorizontal, lineOffset, textIndentInfo.hanging? textIndent: QPointF());
                if (!inlineSize.isAuto) {
                    currentPos += currentLine.textIndent;
                }
            }

            if (charResult.overflowWrap) {
                qreal wordLength = isHorizontal ? wordAdvance.x() : wordAdvance.y();
                if (!inlineSize.isAuto && wordLength > inlineSize.customValue) {
                    // Word is too large, so we try to add it in
                    // max-width-friendly-chunks.
                    wordAdvance = QPointF();
                    wordLength = 0;
                    QVector<int> partialWord;
                    currentLine.firstLine = firstLine;
                    Q_FOREACH (const int i, wordIndices) {
                        wordAdvance += result.at(i).advance;
                        wordLength = isHorizontal ? wordAdvance.x() : wordAdvance.y();
                        if (wordLength <= inlineSize.customValue) {
                            partialWord.append(i);
                        } else {
                            addWordToLine(result, currentPos, partialWord, currentLine, ltr);

                            finalizeLine(result,
                                         currentPos,
                                         currentLine,
                                         lineOffset,
                                         anchor,
                                         writingMode,
                                         ltr,
                                         !inlineSize.isAuto,
                                         false);
                            lineBoxes.append(currentLine);
                            currentLine.clearAndAdjust(isHorizontal, lineOffset, textIndentInfo.hanging? textIndent: QPointF());
                            if (!inlineSize.isAuto) {
                                currentPos += currentLine.textIndent;
                            }
                            wordAdvance = result.at(i).advance;
                            partialWord.append(i);
                        }
                    }
                    wordIndices = partialWord;
                }
            }
            addWordToLine(result, currentPos, wordIndices, currentLine, ltr);
        }

        if (charResult.breakType == HardBreak) {
            finalizeLine(result,
                         currentPos,
                         currentLine,
                         lineOffset,
                         anchor,
                         writingMode,
                         ltr,
                         !inlineSize.isAuto,
                         false);
            lineBoxes.append(currentLine);
            bool indentLine = textIndentInfo.hanging? false: textIndentInfo.eachLine;
            currentLine.clearAndAdjust(isHorizontal, lineOffset, indentLine? textIndent: QPointF());
            if (!inlineSize.isAuto) {
                currentPos += currentLine.textIndent;
            }
        }

        if (currentLine.lastLine) {
            if (!wordIndices.isEmpty()) {
                addWordToLine(result, currentPos, wordIndices, currentLine, ltr);
            }
            finalizeLine(result,
                         currentPos,
                         currentLine,
                         lineOffset,
                         anchor,
                         writingMode,
                         ltr,
                         !inlineSize.isAuto,
                         false);
            lineBoxes.append(currentLine);
        }
    }
    debugFlake << "Linebreaking finished";
    return lineBoxes;
}

bool getFirstPosition(QPointF &firstPoint,
                      QPainterPath p,
                      QRectF wordBox,
                      QPointF terminator,
                      KoSvgText::WritingMode writingMode,
                      bool ltr) {
    QVector<QPointF> candidatePositions;
    QRectF word = wordBox.normalized();
    qreal precision = 1.0; // floating point maths can be imprecise. TODO: make smaller?.
    word.translate(-wordBox.topLeft());
    QPointF terminatorAdjusted = terminator;
    Q_FOREACH(const QPolygonF polygon, p.toFillPolygons()) {
        QVector<QLineF> offsetPoly;
        for(int i = 0; i < polygon.size()-1; i++) {
            QLineF line;
            line.setP1(polygon.at(i));
            line.setP2(polygon.at(i+1));

            if (line.angle() == 0.0 || line.angle() == 180.0) {
                qreal offset = word.center().y() + precision;
                offsetPoly.append(line.translated(0, offset));
                offsetPoly.append(line.translated(0, -offset));
            } else if (line.angle() == 90.0 || line.angle() == 270.0) {
                qreal offset = word.center().x() + precision;
                offsetPoly.append(line.translated(offset, 0));
                offsetPoly.append(line.translated(-offset, 0));
            } else {
                qreal tAngle = fmod(line.angle(), 180.0);
                QPointF cPos = tAngle > 90? line.center() + QPointF(-word.center().x(), word.center().y()): line.center() + word.center();
                qreal offset = kisDistanceToLine(cPos, line) + precision;
                const QPointF vectorT(qCos(qDegreesToRadians(tAngle)), -qSin(qDegreesToRadians(tAngle)));
                QPointF vectorN(-vectorT.y(), vectorT.x());
                QPointF offsetP = QPointF() - (0.0 * vectorT) + (offset * vectorN);
                offsetPoly.append(line.translated(offsetP));
                offsetPoly.append(line.translated(-offsetP));
            }
        }
        if (writingMode == KoSvgText::HorizontalTB) {
            terminatorAdjusted = terminator + word.center();
            QLineF top(polygon.boundingRect().topLeft(), polygon.boundingRect().topRight());
            offsetPoly.append(top.translated(0, terminatorAdjusted.y()));
        } else if (writingMode == KoSvgText::VerticalRL) {
            terminatorAdjusted = terminator - word.center();
            QLineF top(terminatorAdjusted.x(), polygon.boundingRect().top(),
                       terminatorAdjusted.x(), polygon.boundingRect().bottom());
            offsetPoly.append(top);
        } else{
            terminatorAdjusted = terminator + word.center();
            QLineF top(terminatorAdjusted.x(), polygon.boundingRect().top(),
                       terminatorAdjusted.x(), polygon.boundingRect().bottom());
            offsetPoly.append(top);
        }
        for (int i=0; i < offsetPoly.size(); i++) {
            QLineF line = offsetPoly.at(i);
            for (int j=i; j< offsetPoly.size(); j++){
                QLineF line2 = offsetPoly.at(j);
                QPointF intersectPoint;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
                QLineF::IntersectType intersect = line.intersects(line2, &intersectPoint);
#else
                QLineF::IntersectType intersect = line.intersect(line2, &intersectPoint);
#endif
                if (intersect != QLineF::NoIntersection) {
                    // should proly handle 'reflex' vertices better.
                    if (!p.contains(intersectPoint)) {
                        continue;
                    }
                    if(!p.contains(word.translated(intersectPoint-word.center()))) {
                        continue;
                    }
                    if (!candidatePositions.contains(intersectPoint)) {
                        candidatePositions.append(intersectPoint);
                    }
                }
            }
        }
    }
    if (candidatePositions.isEmpty()) {
        return false;
    }

    QPointF firstPointC = writingMode == KoSvgText::VerticalRL? p.boundingRect().bottomLeft(): p.boundingRect().bottomRight();
    Q_FOREACH(const QPointF candidate, candidatePositions) {
        if (writingMode == KoSvgText::HorizontalTB) {
            if (terminatorAdjusted.y() - candidate.y() < precision) {

                if (firstPointC.y() - candidate.y() > precision) {
                    firstPointC = candidate;
                } else if (firstPointC.y() - candidate.y() > -precision) {
                    if (ltr) {
                        if (candidate.x() < firstPointC.x()) {
                            firstPointC = candidate;
                        }
                    } else {
                        if (candidate.x() > firstPointC.x()) {
                            firstPointC = candidate;
                        }
                    }
                }
            }
        } else if (writingMode == KoSvgText::VerticalRL) {
            if (terminatorAdjusted.x() - candidate.x() >= -precision) {

                if (firstPointC.x() - candidate.x() < -precision) {
                    firstPointC = candidate;
                } else if (firstPointC.x() - candidate.x() < precision) {
                    if (ltr) {
                        if (candidate.y() < firstPointC.y()) {
                            firstPointC = candidate;
                        }
                    } else {
                        if (candidate.y() > firstPointC.y()) {
                            firstPointC = candidate;
                        }
                    }
                }
            }
        } else {
            if (terminatorAdjusted.x() - candidate.x() < precision) {

                if (firstPointC.x() - candidate.x() > precision) {
                    firstPointC = candidate;
                } else if (firstPointC.x() - candidate.x() > -precision) {
                    if (ltr) {
                        if (candidate.y() < firstPointC.y()) {
                            firstPointC = candidate;
                        }
                    } else {
                        if (candidate.y() > firstPointC.y()) {
                            firstPointC = candidate;
                        }
                    }
                }
            }
        }
    }
    if (!p.contains(firstPointC)) {
        return false;
    }
    firstPointC -= word.center();
    firstPointC -= wordBox.topLeft();
    firstPoint = firstPointC;

    return true;
}

static bool pointLessThan(const QPointF &a, const QPointF &b)
{
    return a.x() < b.x();
}

static bool pointLessThanVertical(const QPointF &a, const QPointF &b)
{
    return a.y() < b.y();
}

QVector<QLineF> findLineBoxesForFirstPos(QPainterPath shape, QPointF firstPos, QRectF wordBox, KoSvgText::WritingMode writingMode) {
    QVector<QLineF> lines;

    QLineF baseLine;
    QPointF lineTop;
    QPointF lineBottom;

    if (writingMode == KoSvgText::HorizontalTB) {
        baseLine = QLineF(shape.boundingRect().left()-5, firstPos.y(), shape.boundingRect().right()+5, firstPos.y());
        lineTop = QPointF(0, wordBox.top());
        lineBottom = QPointF(0, wordBox.bottom());
    } else {
        baseLine = QLineF(firstPos.x(), shape.boundingRect().top()-5, firstPos.x(), shape.boundingRect().bottom()+5);
        if (writingMode == KoSvgText::VerticalRL) {
            lineTop = QPointF(wordBox.left(), 0);
            lineBottom = QPointF(wordBox.right(), 0);
        } else {
            lineTop = QPointF(wordBox.right(), 0);
            lineBottom = QPointF(wordBox.left(), 0);
        }
    }

    QPolygonF polygon = shape.toFillPolygon();
    QList<QPointF> intersects;
    QLineF topLine = baseLine.translated(lineTop);
    QLineF bottomLine = baseLine.translated(lineBottom);
    for(int i = 0; i < polygon.size()-1; i++) {
        QLineF line(polygon.at(i), polygon.at(i+1));
        bool addedA = false;
        QPointF intersectA;
        QPointF intersectB;
        QPointF intersect;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
        if (topLine.intersects(line, &intersect) == QLineF::BoundedIntersection) {
#else
        if (topLine.intersect(line, &intersect) == QLineF::BoundedIntersection) {
#endif
            intersectA = intersect-lineTop;
            intersects.append(intersectA);
            addedA = true;
        }
#if (QT_VERSION >= QT_VERSION_CHECK(5, 13, 0))
        if (bottomLine.intersects(line, &intersect) == QLineF::BoundedIntersection) {
#else
        if (bottomLine.intersect(line, &intersect) == QLineF::BoundedIntersection) {
#endif
            intersectB = intersect-lineBottom;
            if (intersectA != intersectB || !addedA) {
                intersects.append(intersectB);
            }
        }
    }
    if (!intersects.isEmpty()) {
        intersects.append(baseLine.p1());
        intersects.append(baseLine.p2());
    }
    if (writingMode == KoSvgText::HorizontalTB) {
        std::sort(intersects.begin(), intersects.end(), pointLessThan);
    } else {
        std::sort(intersects.begin(), intersects.end(), pointLessThanVertical);
    }


    for (int i = 0; i< intersects.size()-1; i++) {
        QLineF line(intersects.at(i), intersects.at(i+1));

        if (!(shape.contains(line.translated(lineTop).center())
              && shape.contains(line.translated(lineBottom).center()))
                || line.length() == 0) {
            continue;
        }

        QRectF lineBox = QRectF(line.p1() + lineTop, line.p2() + lineBottom).normalized();

        QVector<QPointF> relevant;
        for(int i = 0; i < polygon.size()-1; i++) {

            QLineF edgeLine(polygon.at(i), polygon.at(i+1));

            if (lineBox.contains(polygon.at(i))) {
                relevant.append(polygon.at(i));
            }
        }
        qreal start = writingMode == KoSvgText::HorizontalTB? lineBox.left(): lineBox.top();
        qreal end = writingMode == KoSvgText::HorizontalTB? lineBox.right(): lineBox.bottom();
        for(int j = 0; j < relevant.size(); j++) {
            QPointF current = relevant.at(j);

            if (writingMode == KoSvgText::HorizontalTB) {
                if (current.x() < line.center().x()) {
                    start = qMax(current.x(), start);
                } else if (current.x() > line.center().x()) {
                    end = qMin(current.x(), end);
                }
            } else {
                if (current.y() < line.center().y()) {
                    start = qMax(current.y(), start);
                } else if (current.y() > line.center().y()) {
                    end = qMin(current.y(), end);
                }
            }
        }
        if (writingMode == KoSvgText::HorizontalTB) {

            QLineF newLine(start, line.p1().y(), end, line.p2().y());
            if (!lines.isEmpty()) {
                if (lines.last().p2() == intersects.at(i)) {
                    newLine.setP1(lines.last().p1());
                    lines.removeLast();
                }
            }
            lines.append(newLine);
        } else {
            QLineF newLine(line.p1().x(), start, line.p2().x(), end);
            if (!lines.isEmpty()) {
                if (lines.last().p2() == intersects.at(i)) {
                    newLine.setP1(lines.last().p1());
                    lines.removeLast();
                }
            }
            lines.append(newLine);
        }
    }

    return lines;
}

void getEstimatedHeight(QVector<CharacterResult> &result, int index, QRectF &wordBox, QRectF boundingBox, KoSvgText::WritingMode writingMode) {
    bool isHorizontal = writingMode == KoSvgText::HorizontalTB;
    QPointF totalAdvance = wordBox.bottomRight() - wordBox.topLeft();
    qreal maxAscent = isHorizontal? fabs(wordBox.top()): fabs(wordBox.right());
    qreal maxDescent = isHorizontal? fabs(wordBox.bottom()): fabs(wordBox.left());

    for (int i=index; i<result.size(); i++) {
        if (!result.at(i).addressable || result.at(i).hidden) {
            continue;
        }
        totalAdvance += result.at(i).advance;
        if ((totalAdvance.x() > boundingBox.width() && isHorizontal) ||
                (totalAdvance.y() > boundingBox.height() && !isHorizontal)) {
            break;
        }
        maxAscent = qMax(fabs(result.at(i).ascent-result.at(i).halfLeading), maxAscent);
        maxDescent = qMax(fabs(result.at(i).descent+result.at(i).halfLeading), maxDescent);
    }
    if (writingMode == KoSvgText::HorizontalTB) {
        wordBox.setTop(-maxAscent);
        wordBox.setBottom(maxDescent);
    } else {
        // vertical lr has top at the right even though block flow is also to the right.
        wordBox.setRight(maxAscent);
        wordBox.setLeft(-maxDescent);
    }
}

KoSvgText::TextAnchor textAnchorForTextAlign(KoSvgText::TextAlign align, KoSvgText::TextAlign alignLast, bool ltr) {
    KoSvgText::TextAlign compare = align;
    if (align == KoSvgText::AlignJustify) {
        compare = alignLast;
    }
    if (compare == KoSvgText::AlignStart) {
        return KoSvgText::AnchorStart;
    } else if (compare == KoSvgText::AlignCenter) {
        return KoSvgText::AnchorMiddle;
    } else if (compare == KoSvgText::AlignEnd) {
        return KoSvgText::AnchorEnd;
    } else if (compare == KoSvgText::AlignLeft) {
        return ltr? KoSvgText::AnchorStart: KoSvgText::AnchorEnd;
    } else if (compare == KoSvgText::AlignRight) {
        return ltr? KoSvgText::AnchorEnd: KoSvgText::AnchorStart;
    } else if (align == KoSvgText::AlignJustify) {
        return KoSvgText::AnchorMiddle;
    }
    return KoSvgText::AnchorStart;
}

QVector<LineBox> KoSvgTextShape::Private::flowTextInShapes(const KoSvgTextProperties &properties,
                                               const QMap<int, int> &logicalToVisual,
                                               QVector<CharacterResult> &result,
                                               QList<QPainterPath> shapes) {
    QVector<LineBox> lineBoxes;
    KoSvgText::WritingMode writingMode = KoSvgText::WritingMode(properties.propertyOrDefault(KoSvgTextProperties::WritingModeId).toInt());
    KoSvgText::Direction direction = KoSvgText::Direction(properties.propertyOrDefault(KoSvgTextProperties::DirectionId).toInt());
    bool ltr = direction == KoSvgText::DirectionLeftToRight;
    bool isHorizontal = writingMode == KoSvgText::HorizontalTB;
    KoSvgText::TextAlign align = KoSvgText::TextAlign(properties.propertyOrDefault(KoSvgTextProperties::TextAlignAllId).toInt());
    KoSvgText::TextAlign alignLast = KoSvgText::TextAlign(properties.propertyOrDefault(KoSvgTextProperties::TextAlignLastId).toInt());
    KoSvgText::TextAnchor anchor = textAnchorForTextAlign(align, alignLast, ltr);

    QPointF textIndent; ///< The textIndent.
    KoSvgText::TextIndentInfo textIndentInfo = properties.propertyOrDefault(KoSvgTextProperties::TextIndentId).value<KoSvgText::TextIndentInfo>();

    QVector<int> wordIndices; ///< 'word' in this case meaning characters
                              ///< inbetween softbreaks.
    QRectF wordBox; ///< Approximated box of the currrent word;
    QPointF wordAdvance;

    LineBox currentLine;
    bool firstLine = true; ///< First line will be created proper after we get our first wordbox, this tracks if it's the first.
    bool indentLine = true;

    QPointF currentPos = writingMode == KoSvgText::VerticalRL? shapes.at(0).boundingRect().topRight()
                                                             :shapes.at(0).boundingRect().topLeft(); ///< Current position with advances of each character.
    QPointF lineOffset = currentPos; ///< Current line offset.

    QListIterator<int> it(logicalToVisual.keys());
    QListIterator<QPainterPath> shapesIt(shapes);
    if (shapes.isEmpty()) {
        return lineBoxes;
    }
    QPainterPath currentShape;
    while (it.hasNext()) {
        int index = it.next();
        CharacterResult charResult = result.at(index);
        if (!charResult.addressable) {
            continue;
        }

        bool softBreak = false; ///< Whether to break a line.
        bool doNotCountAdvance =
            ((charResult.lineEnd != NoChange)
             && !(currentLine.isEmpty() && wordIndices.isEmpty()));
        if (!doNotCountAdvance) {
            if (wordIndices.isEmpty()) {
                wordBox = charResult.boundingBox;
                wordAdvance = charResult.advance;
            } else {
                wordBox |= charResult.boundingBox.translated(wordAdvance);
                wordAdvance += charResult.advance;
            }
        }
        wordIndices.append(index);
        currentLine.lastLine = !it.hasNext();
        if (currentLine.lastLine) {
            currentLine.justifyLine = alignLast == KoSvgText::AlignJustify;
        }

        if (charResult.breakType != NoBreak || currentLine.lastLine) {
            if (currentLine.chunks.isEmpty() || currentLine.lastLine) {
                softBreak = true;
            }

            for (int i = currentLine.currentChunk; i < currentLine.chunks.size(); i++) {
                if (i == -1) {
                    currentLine.currentChunk = 0;
                    i = 0;
                }
                QLineF line = currentLine.chunks.value(i).length;
                qreal lineLength = isHorizontal ? (currentPos - line.p1() + wordAdvance).x()
                                                : (currentPos - line.p1() + wordAdvance).y();
                if (qRound((abs(lineLength) - line.length())) > 0) {
                    if (i == currentLine.chunks.size()-1) {
                        softBreak = true;
                        break;
                    } else {
                        QLineF nextLine = currentLine.chunks.value(i+1).length;
                        if (isHorizontal) {
                            currentPos.setX(ltr? qMax(nextLine.p1().x(), currentPos.x()):
                                                 qMin(nextLine.p1().x(), currentPos.x()));
                        } else {
                            currentPos.setY(nextLine.p1().y());
                        }
                    }
                } else {
                    currentLine.currentChunk = i;
                    addWordToLine(result, currentPos, wordIndices, currentLine, ltr);
                    break;
                }
            }
        }

        if (softBreak) {
            if (!currentLine.isEmpty()) {
                finalizeLine(result, currentPos, currentLine, lineOffset, anchor, writingMode, ltr, true, true);
                lineBoxes.append(currentLine);
                firstLine = false;
                indentLine = false;
            }
            // Not adding indent to the (first) word box means it'll overflow if there's no room,
            // but being too strict might end with the whole text dissapearing. Given Krita's text layout is
            // in an interactive context, ugly result might be more communicative than all text dissapearing.
            bool ind = textIndentInfo.hanging? !indentLine: indentLine;
            QPointF indent = ind? textIndent: QPointF();
            bool foundFirst = false;
            bool needNewLine = true;
            // add text indent to wordbox.
            getEstimatedHeight(result, index, wordBox, currentShape.boundingRect(), writingMode);
            if (!currentShape.isEmpty()) {
                // we're going to try and get an offset line first before trying get first pos.
                // This gives more stable results on curved shapes.
                currentPos -= writingMode == KoSvgText::VerticalRL? wordBox.topRight(): wordBox.topLeft();
                currentLine = LineBox(findLineBoxesForFirstPos(currentShape, currentPos, wordBox, writingMode), ltr, indent);
                qreal length = isHorizontal? wordBox.width(): wordBox.height();
                for (int i = 0; i < currentLine.chunks.size(); i++) {
                    if (currentLine.chunks.at(i).length.length() > length) {
                        currentLine.currentChunk = i;
                        foundFirst = true;
                        needNewLine = false;
                        break;
                    }
                }
            }
            /*
             * In theory we could have overflow wrap for shapes, but it'd require either generalizing
             * the line-filling portion above and this new line seeking portion, or somehow reverting
             * the itterator over the results to be on the last fitted glyph (which'd still require
             * generalizing the line-filling portion about), and I am unsure how to do that.
             * Either way, this place here is where you'd check for overflow wrap.
             */
            while(!foundFirst) {
                foundFirst = getFirstPosition(currentPos, currentShape, wordBox, lineOffset, writingMode, ltr);
                if (foundFirst || !shapesIt.hasNext()) {
                    break;
                }
                currentShape = shapesIt.next();
                qreal textIdentValue = textIndentInfo.value;
                if (isHorizontal) {
                    if (textIndentInfo.isPercentage) {
                        textIndent *= currentShape.boundingRect().width();
                    }
                    textIndent = QPointF(textIdentValue, 0);
                } else {
                    if (textIndentInfo.isPercentage) {
                        textIndent *= currentShape.boundingRect().height();
                    }
                    textIndent = QPointF(0, textIdentValue);
                }
                bool ind = textIndentInfo.hanging? !indentLine: indentLine;
                indent = ind? textIndent: QPointF();
                currentPos = writingMode == KoSvgText::VerticalRL? currentShape.boundingRect().topRight():currentShape.boundingRect().topLeft();
                lineOffset = currentPos;
            }
            if (foundFirst) {
                if (needNewLine) {
                    currentLine = LineBox(findLineBoxesForFirstPos(currentShape, currentPos, wordBox, writingMode), ltr, indent);
                    // We could set this to find the first fitting width, but it's better to try and improve the precision of the firstpos algorithm,
                    // as this gives more stable results.
                    currentLine.setCurrentChunkForPos(currentPos, isHorizontal);
                }
                currentLine.firstLine = firstLine;
                currentLine.expectedLineTop = isHorizontal? fabs(wordBox.top()):
                                                            writingMode == KoSvgText::VerticalRL? fabs(wordBox.right()): fabs(wordBox.left());
                currentLine.justifyLine = align == KoSvgText::AlignJustify;
                currentPos = currentLine.chunk().length.p1() + indent;
                lineOffset = currentPos;
                addWordToLine(result, currentPos, wordIndices, currentLine, ltr);
            } else {
                currentLine = LineBox();
                Q_FOREACH (const int j, wordIndices) {
                    result[j].hidden = true;
                }
            }
        }

        if (charResult.breakType == HardBreak) {
            finalizeLine(result, currentPos, currentLine, lineOffset, anchor, writingMode, ltr, true, true);
            lineBoxes.append(currentLine);
            currentLine = LineBox();
            indentLine = textIndentInfo.hanging? false: textIndentInfo.eachLine;
        }
    }
    finalizeLine(result, currentPos, currentLine, lineOffset, anchor, writingMode, ltr, true, true);
    lineBoxes.append(currentLine);
    return lineBoxes;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void KoSvgTextShape::Private::applyTextLength(const KoShape *rootShape,
                                              QVector<CharacterResult> &result,
                                              int &currentIndex,
                                              int &resolvedDescendentNodes,
                                              bool isHorizontal)
{
    const KoSvgTextChunkShape *chunkShape = dynamic_cast<const KoSvgTextChunkShape *>(rootShape);
    KIS_SAFE_ASSERT_RECOVER_RETURN(chunkShape);

    int i = currentIndex;
    int j = i + chunkShape->layoutInterface()->numChars(true);
    int resolvedChildren = 0;

    Q_FOREACH (KoShape *child, chunkShape->shapes()) {
        applyTextLength(child, result, currentIndex, resolvedChildren, isHorizontal);
    }
    // Raqm handles bidi reordering for us, but this algorithm does not
    // anticipate that, so we need to keep track of which typographic item
    // belongs where.
    QMap<int, int> visualToLogical;
    if (!chunkShape->layoutInterface()->textLength().isAuto) {
        qreal a = 0.0;
        qreal b = 0.0;
        int n = 0;
        for (int k = i; k < j; k++) {
            if (result.at(k).addressable) {
                if (result.at(k).visualIndex > -1) {
                    visualToLogical.insert(result.at(k).visualIndex, k);
                }
                // if character is linebreak, return;

                qreal pos = result.at(k).finalPosition.x();
                qreal advance = result.at(k).advance.x();
                if (!isHorizontal) {
                    pos = result.at(k).finalPosition.y();
                    advance = result.at(k).advance.y();
                }
                if (k == i) {
                    a = qMin(pos, pos + advance);
                    b = qMax(pos, pos + advance);
                } else {
                    a = qMin(a, qMin(pos, pos + advance));
                    b = qMax(b, qMax(pos, pos + advance));
                }
                if (!result.at(k).textLengthApplied) {
                    n += 1;
                }
            }
        }
        n += resolvedChildren;
        bool spacingAndGlyphs = (chunkShape->layoutInterface()->lengthAdjust() == KoSvgText::LengthAdjustSpacingAndGlyphs);
        if (!spacingAndGlyphs) {
            n -= 1;
        }
        const qreal delta = chunkShape->layoutInterface()->textLength().customValue - (b - a);

        const QPointF d = isHorizontal ? QPointF(delta / n, 0) : QPointF(0, delta / n);

        QPointF shift;
        bool secondTextLengthApplied = false;
        Q_FOREACH (int k, visualToLogical.keys()) {
            CharacterResult cr = result[visualToLogical.value(k)];
            if (cr.addressable) {
                cr.finalPosition += shift;
                if (spacingAndGlyphs) {
                    QPointF scale(d.x() != 0 ? (d.x() / cr.advance.x()) + 1 : 1.0, d.y() != 0 ? (d.y() / cr.advance.y()) + 1 : 1.0);
                    QTransform tf = QTransform::fromScale(scale.x(), scale.y());
                    cr.path = tf.map(cr.path);
                    cr.advance = tf.map(cr.advance);
                    cr.boundingBox = tf.mapRect(cr.boundingBox);
                }
                bool last = spacingAndGlyphs ? false : k == visualToLogical.keys().last();

                if (!(cr.textLengthApplied && secondTextLengthApplied) && !last) {
                    shift += d;
                }
                secondTextLengthApplied = cr.textLengthApplied;
                cr.textLengthApplied = true;
            }
            result[visualToLogical.value(k)] = cr;
        }
        resolvedDescendentNodes += 1;

        // apply the shift to all consequetive chars as long as they don't start
        // a new chunk.
        int lastVisualValue = visualToLogical.keys().last();
        visualToLogical.clear();

        for (int k = j; k < result.size(); k++) {
            if (result.at(k).anchored_chunk) {
                break;
            }
            visualToLogical.insert(result.at(k).visualIndex, k);
        }
        // And also backwards for rtl.
        for (int k = i; k > -1; k--) {
            visualToLogical.insert(result.at(k).visualIndex, k);
            if (result.at(k).anchored_chunk) {
                break;
            }
        }
        Q_FOREACH (int k, visualToLogical.keys()) {
            if (k > lastVisualValue) {
                result[visualToLogical.value(k)].finalPosition += shift;
            }
        }
    }

    currentIndex = j;
}

/**
 * @brief KoSvgTextShape::Private::computeFontMetrics
 * This function handles computing the baselineOffsets
 */

void KoSvgTextShape::Private::computeFontMetrics( // NOLINT(readability-function-cognitive-complexity)
    const KoShape *rootShape,
    const QMap<int, int> &parentBaselineTable,
    qreal parentFontSize,
    QPointF superScript,
    QPointF subScript,
    QVector<CharacterResult> &result,
    QVector<LineBox> lineBoxes,
    int &currentIndex,
    qreal res,
    bool isHorizontal)
{
    const KoSvgTextChunkShape *chunkShape = dynamic_cast<const KoSvgTextChunkShape *>(rootShape);
    KIS_SAFE_ASSERT_RECOVER_RETURN(chunkShape);

    QMap<int, int> baselineTable;
    const int i = currentIndex;
    const int j = qMin(i + chunkShape->layoutInterface()->numChars(true), result.size());

    KoSvgTextProperties properties = chunkShape->textProperties();

    const qreal fontSize = properties.propertyOrDefault(KoSvgTextProperties::FontSizeId).toReal();
    const qreal baselineShift = properties.property(KoSvgTextProperties::BaselineShiftValueId).toReal() * fontSize;
    QPointF baselineShiftTotal;
    KoSvgText::BaselineShiftMode baselineShiftMode = KoSvgText::BaselineShiftMode(properties.property(KoSvgTextProperties::BaselineShiftModeId).toInt());

    if (baselineShiftMode == KoSvgText::ShiftSuper) {
        baselineShiftTotal = isHorizontal ? superScript : QPointF(-superScript.y(), superScript.x());
    } else if (baselineShiftMode == KoSvgText::ShiftSub) {
        baselineShiftTotal = isHorizontal ? subScript : QPointF(-subScript.y(), subScript.x());
    } else if (baselineShiftMode == KoSvgText::ShiftPercentage) {
        baselineShiftTotal = isHorizontal ? QPointF(0, baselineShift) : QPointF(-baselineShift, 0);
    }

    QVector<int> lengths;
    const QFont::Style style = QFont::Style(properties.propertyOrDefault(KoSvgTextProperties::FontStyleId).toInt());
    KoSvgText::AutoValue fontSizeAdjust = properties.propertyOrDefault(KoSvgTextProperties::FontSizeAdjustId).value<KoSvgText::AutoValue>();
    if (properties.hasProperty(KoSvgTextProperties::KraTextVersionId)) {
        fontSizeAdjust.isAuto = (properties.property(KoSvgTextProperties::KraTextVersionId).toInt() < 3);
    }
    const std::vector<FT_FaceUP> faces = KoFontRegistry::instance()->facesForCSSValues(
        properties.property(KoSvgTextProperties::FontFamiliesId).toStringList(),
        lengths,
        properties.fontAxisSettings(),
        QString(),
        static_cast<quint32>(res),
        static_cast<quint32>(res),
        fontSize,
        fontSizeAdjust.isAuto ? 1.0 : fontSizeAdjust.customValue,
        properties.propertyOrDefault(KoSvgTextProperties::FontWeightId).toInt(),
        properties.propertyOrDefault(KoSvgTextProperties::FontStretchId).toInt(),
        style != QFont::StyleNormal);

    hb_font_t_up font(hb_ft_font_create_referenced(faces.front().data()));
    const qreal freetypePixelsToPt = (1.0 / 64.0) * (72. / res);

    hb_direction_t dir = HB_DIRECTION_LTR;
    if (!isHorizontal) {
        dir = HB_DIRECTION_TTB;
    }
    hb_script_t script = HB_SCRIPT_UNKNOWN;
    KoSvgText::Baseline dominantBaseline = KoSvgText::Baseline(properties.property(KoSvgTextProperties::DominantBaselineId).toInt());

    hb_position_t baseline = 0;
    if (dominantBaseline == KoSvgText::BaselineResetSize && parentFontSize > 0) {
        baselineTable = parentBaselineTable;
        qreal multiplier = 1.0 / parentFontSize * fontSize;
        Q_FOREACH (int key, baselineTable.keys()) {
            baselineTable.insert(key, static_cast<int>(baselineTable.value(key) * multiplier));
        }
        dominantBaseline = KoSvgText::BaselineAuto;
    } else if (dominantBaseline == KoSvgText::BaselineNoChange) {
        baselineTable = parentBaselineTable;
        dominantBaseline = KoSvgText::BaselineAuto;
    } else {
        if (hb_version_atleast(4, 0, 0)) {
            hb_ot_layout_get_baseline_with_fallback(font.data(), HB_OT_LAYOUT_BASELINE_TAG_ROMAN, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineAlphabetic, baseline);
            hb_ot_layout_get_baseline_with_fallback(font.data(), HB_OT_LAYOUT_BASELINE_TAG_MATH, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineMathematical, baseline);
            hb_ot_layout_get_baseline_with_fallback(font.data(), HB_OT_LAYOUT_BASELINE_TAG_HANGING, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineHanging, baseline);
            hb_ot_layout_get_baseline_with_fallback(font.data(), HB_OT_LAYOUT_BASELINE_TAG_IDEO_FACE_CENTRAL, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineCentral, baseline);
            hb_ot_layout_get_baseline_with_fallback(font.data(), HB_OT_LAYOUT_BASELINE_TAG_IDEO_EMBOX_BOTTOM_OR_LEFT, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineIdeographic, baseline);
            if (isHorizontal) {
                hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_X_HEIGHT, &baseline);
                baselineTable.insert(KoSvgText::BaselineMiddle, static_cast<int>((baseline - baselineTable.value(KoSvgText::BaselineAlphabetic)) * 0.5));

            } else {
                baselineTable.insert(KoSvgText::BaselineMiddle, baselineTable.value(KoSvgText::BaselineCentral));
            }
        } else {
            hb_ot_layout_get_baseline(font.data(), HB_OT_LAYOUT_BASELINE_TAG_ROMAN, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineAlphabetic, baseline);
            hb_ot_layout_get_baseline(font.data(), HB_OT_LAYOUT_BASELINE_TAG_MATH, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineMathematical, baseline);
            hb_ot_layout_get_baseline(font.data(), HB_OT_LAYOUT_BASELINE_TAG_HANGING, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineHanging, baseline);
            hb_ot_layout_get_baseline(font.data(), HB_OT_LAYOUT_BASELINE_TAG_IDEO_FACE_CENTRAL, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineCentral, baseline);
            hb_ot_layout_get_baseline(font.data(), HB_OT_LAYOUT_BASELINE_TAG_IDEO_EMBOX_BOTTOM_OR_LEFT, dir, script, HB_TAG_NONE, &baseline);
            baselineTable.insert(KoSvgText::BaselineIdeographic, baseline);
            if (isHorizontal) {
                hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_X_HEIGHT, &baseline);
                baselineTable.insert(KoSvgText::BaselineMiddle, static_cast<int>((baseline - baselineTable.value(KoSvgText::BaselineAlphabetic)) * 0.5));
            } else {
                baselineTable.insert(KoSvgText::BaselineMiddle, baselineTable.value(KoSvgText::BaselineCentral));
            }
        }
    }

    // Get underline and super/subscripts.
    QPointF newSuperScript;
    QPointF newSubScript;
    if (hb_version_atleast(4, 0, 0)) {
        hb_position_t baseline2 = 0;
        hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_SUPERSCRIPT_EM_X_OFFSET, &baseline);
        hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_SUPERSCRIPT_EM_Y_OFFSET, &baseline2);
        newSuperScript = QPointF(baseline * freetypePixelsToPt, baseline2 * -freetypePixelsToPt);
        hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_SUBSCRIPT_EM_X_OFFSET, &baseline);
        hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_SUBSCRIPT_EM_Y_OFFSET, &baseline2);
        newSubScript = QPointF(baseline * freetypePixelsToPt, baseline2 * freetypePixelsToPt);

        qreal width = 0;
        qreal offset = 0;
        hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_UNDERLINE_SIZE, &baseline);
        width = baseline;
        hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &baseline);
        offset = baseline;
        offset *= -freetypePixelsToPt;
        width *= -freetypePixelsToPt;

        chunkShape->layoutInterface()->setTextDecorationFontMetrics(KoSvgText::DecorationUnderline, offset, width);
        chunkShape->layoutInterface()->setTextDecorationFontMetrics(KoSvgText::DecorationOverline, 0, width);

        hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &baseline);
        width = baseline;
        hb_ot_metrics_get_position_with_fallback(font.data(), HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &baseline);
        width *= -freetypePixelsToPt;
        offset *= -freetypePixelsToPt;
        chunkShape->layoutInterface()->setTextDecorationFontMetrics(KoSvgText::DecorationLineThrough, offset, width);
    } else {
        baseline = 0;
        hb_position_t baseline2 = 0;
        hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_SUPERSCRIPT_EM_X_OFFSET, &baseline);
        hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_SUPERSCRIPT_EM_Y_OFFSET, &baseline2);
        if (baseline2 == 0) {
            newSuperScript = QPointF(0, 0.6 * -fontSize);
        } else {
            newSuperScript = QPointF(baseline * freetypePixelsToPt, baseline2 * -freetypePixelsToPt);
        }
        baseline = 0;
        baseline2 = 0;
        hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_SUBSCRIPT_EM_X_OFFSET, &baseline);
        hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_SUBSCRIPT_EM_Y_OFFSET, &baseline2);
        // Subscript should be 'added' onto the baseline'.
        if (baseline2 == 0) {
            newSubScript = QPointF(0, 0.2 * fontSize);
        } else {
            newSubScript = QPointF(baseline * freetypePixelsToPt, baseline2 * freetypePixelsToPt);
        }

        qreal width = 0;
        qreal offset = 0;
        const double fallbackThickness =
            faces.front()->underline_thickness * (faces.front()->size->metrics.y_scale / 65535.0);
        hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_UNDERLINE_SIZE, &baseline);
        width = qMax<double>(baseline, fallbackThickness);

        hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &baseline);
        offset = baseline;
        offset *= -freetypePixelsToPt;
        width *= freetypePixelsToPt;

        chunkShape->layoutInterface()->setTextDecorationFontMetrics(KoSvgText::DecorationUnderline, offset, width);
        chunkShape->layoutInterface()->setTextDecorationFontMetrics(KoSvgText::DecorationOverline, 0, width);

        hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &baseline);
        width = qMax<double>(baseline, fallbackThickness);
        hb_ot_metrics_get_position(font.data(), HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &baseline);
        if (baseline == 0) {
            offset = baselineTable.value(KoSvgText::BaselineCentral);
        }
        width *= freetypePixelsToPt;
        offset *= -freetypePixelsToPt;

        chunkShape->layoutInterface()->setTextDecorationFontMetrics(KoSvgText::DecorationLineThrough, offset, width);
    }

    Q_FOREACH (KoShape *child, chunkShape->shapes()) {
        computeFontMetrics(child, baselineTable, fontSize, newSuperScript, newSubScript, result, lineBoxes, currentIndex, res, isHorizontal);
    }

    KoSvgText::Baseline baselineAdjust = KoSvgText::Baseline(properties.property(KoSvgTextProperties::AlignmentBaselineId).toInt());

    if (baselineAdjust == KoSvgText::BaselineDominant) {
        baselineAdjust = dominantBaseline;
    }
    if (baselineAdjust == KoSvgText::BaselineAuto || baselineAdjust == KoSvgText::BaselineUseScript) {
        // UseScript got deprecated in CSS-Inline-3.
        if (isHorizontal) {
            baselineAdjust = KoSvgText::BaselineAlphabetic;
        } else {
            baselineAdjust = KoSvgText::BaselineMiddle;
        }
    }

    LineBox relevantLine;
    Q_FOREACH(LineBox lineBox, lineBoxes) {
        Q_FOREACH(LineChunk chunk, lineBox.chunks) {
            if (chunk.chunkIndices.contains(i)) {
                relevantLine = lineBox;
            }
        }
    }

    const int offset = parentBaselineTable.value(baselineAdjust, 0) - baselineTable.value(baselineAdjust, 0);
    QPointF shift;
    if (baselineAdjust == KoSvgText::BaselineTextTop) {
        qreal height;
        shift = relevantLine.baselineTop;
        for (int k = i; k < j; k++) {
            height  = qMax(height, result[k].boundingBox.height());
        }
        shift += isHorizontal? QPointF(0, height):QPointF(height, 0);
    } else if (baselineAdjust == KoSvgText::BaselineTextBottom) {
        shift = relevantLine.baselineBottom;
    } else {
        if (isHorizontal) {
            shift = QPointF(0, offset * -freetypePixelsToPt);
        } else {
            shift = QPointF(offset * freetypePixelsToPt, 0);
        }
    }

    shift += baselineShiftTotal;

    for (int k = i; k < j; k++) {
        CharacterResult cr = result[k];
        cr.cssPosition += shift;
        cr.finalPosition = cr.cssPosition;
        result[k] = cr;
    }

    currentIndex = j;
}

/**
 * @brief KoSvgTextShape::Private::computeTextDecorations
 * Text decorations need to be computed before textPath is applied.
 * This function goes down the tree and computes textDecorations as necessary,
 * bends them to the textPath, strokes them, and then adds them to the node in
 * question.
 */
void KoSvgTextShape::Private::computeTextDecorations( // NOLINT(readability-function-cognitive-complexity)
    const KoShape *rootShape,
    const QVector<CharacterResult> &result,
    const QMap<int, int> &logicalToVisual,
    qreal minimumDecorationThickness,
    KoPathShape *textPath,
    qreal textPathoffset,
    bool side,
    int &currentIndex,
    bool isHorizontal,
    bool ltr,
    bool wrapping)
{
    const KoSvgTextChunkShape *chunkShape = dynamic_cast<const KoSvgTextChunkShape *>(rootShape);
    KIS_SAFE_ASSERT_RECOVER_RETURN(chunkShape);

    const int i = currentIndex;
    const int j = qMin(i + chunkShape->layoutInterface()->numChars(true), result.size());
    using namespace KoSvgText;

    KoPathShape *currentTextPath = nullptr;
    qreal currentTextPathOffset = textPathoffset;
    bool textPathSide = side;
    if (!wrapping) {
        currentTextPath = textPath ? textPath : dynamic_cast<KoPathShape *>(chunkShape->layoutInterface()->textPath());

        if (chunkShape->layoutInterface()->textPath()) {
            textPathSide = chunkShape->layoutInterface()->textOnPathInfo().side == TextPathSideRight;
            if (chunkShape->layoutInterface()->textOnPathInfo().startOffsetIsPercentage) {
                KIS_ASSERT(currentTextPath);
                currentTextPathOffset = currentTextPath->outline().length() * (0.01 * chunkShape->layoutInterface()->textOnPathInfo().startOffset);
            } else {
                currentTextPathOffset = chunkShape->layoutInterface()->textOnPathInfo().startOffset;
            }
        }
    }

    Q_FOREACH (KoShape *child, chunkShape->shapes()) {
        computeTextDecorations(child,
                               result,
                               logicalToVisual,
                               minimumDecorationThickness,
                               currentTextPath,
                               currentTextPathOffset,
                               textPathSide,
                               currentIndex,
                               isHorizontal,
                               ltr,
                               wrapping);
    }

    TextDecorations decor = chunkShape->textProperties().propertyOrDefault(KoSvgTextProperties::TextDecorationLineId).value<TextDecorations>();
    if (decor != DecorationNone && chunkShape->textProperties().hasProperty(KoSvgTextProperties::TextDecorationLineId)) {
        KoSvgTextProperties properties = chunkShape->textProperties();
        TextDecorationStyle style = TextDecorationStyle(properties.propertyOrDefault(KoSvgTextProperties::TextDecorationStyleId).toInt());

        QMap<TextDecoration, QPainterPath> decorationPaths;
        QMap<TextDecoration, QPointF> decorationOffsets;

        decorationPaths.insert(DecorationUnderline, QPainterPath());
        decorationPaths.insert(DecorationOverline, QPainterPath());
        decorationPaths.insert(DecorationLineThrough, QPainterPath());

        Q_FOREACH (TextDecoration type, decorationPaths.keys()) {
            qreal offset = chunkShape->layoutInterface()->getTextDecorationOffset(type);
            decorationOffsets.insert(type, isHorizontal ? QPointF(0, offset) : QPointF(offset, 0));
        }

        QPainterPathStroker stroker;
        stroker.setWidth(qMax(minimumDecorationThickness, chunkShape->layoutInterface()->getTextDecorationWidth(DecorationUnderline)));
        stroker.setCapStyle(Qt::FlatCap);
        if (style == Dotted) {
            QPen pen;
            pen.setStyle(Qt::DotLine);
            stroker.setDashPattern(pen.dashPattern());
        } else if (style == Dashed) {
            QPen pen;
            pen.setStyle(Qt::DashLine);
            stroker.setDashPattern(pen.dashPattern());
        }
        qreal top = 0;
        qreal bottom = 0;
        QPointF currentFinalPos;
        QVector<QRectF> decorationRects;
        QVector<QPointF> firstPos;
        QRectF currentRect;

        for (int k = i; k < j; k++) {
            CharacterResult charResult = result.at(k);

            if (currentTextPath) {
                characterResultOnPath(charResult,
                                      currentTextPath->outline().length(),
                                      currentTextPathOffset,
                                      isHorizontal,
                                      currentTextPath->isClosedSubpath(0));
            }

            if (charResult.hidden || !charResult.addressable) {
                continue;
            }
            if (charResult.anchored_chunk) {
                QPointF fp = isHorizontal ? QPointF(currentRect.x(), currentFinalPos.y()) : QPointF(currentFinalPos.x(), currentRect.y());
                firstPos.append(fp);
                decorationRects.append(currentRect);
                currentRect = QRectF();
            }

            currentFinalPos = charResult.finalPosition;

            QRectF bbox = charResult.path.isEmpty() ? charResult.boundingBox : charResult.path.boundingRect();

            top = isHorizontal ? qMin(top, bbox.top()) : qMax(top, bbox.right());
            bottom = isHorizontal ? qMax(bottom, bbox.bottom()) : qMin(bottom, bbox.left());

            currentRect |= bbox.translated(charResult.finalPosition);
        }
        decorationRects.append(currentRect);
        QPointF fp = isHorizontal ? QPointF(currentRect.x(), currentFinalPos.y()) : QPointF(currentFinalPos.x(), currentRect.y());
        firstPos.append(fp);

        // Computing the various offsets from the 'top' & 'bottom' values.

        bool underlineOverlineFlipped = false;
        if (isHorizontal) {
            decorationOffsets[DecorationOverline] = QPointF(0, top);
            TextDecorationUnderlinePosition underlinePosH =
                TextDecorationUnderlinePosition(properties.propertyOrDefault(KoSvgTextProperties::TextDecorationPositionHorizontalId).toInt());
            if (underlinePosH == UnderlineUnder) {
                decorationOffsets[DecorationUnderline] = QPointF(0, bottom);
            }
        } else {
            TextDecorationUnderlinePosition underlinePosV =
                TextDecorationUnderlinePosition(properties.propertyOrDefault(KoSvgTextProperties::TextDecorationPositionVerticalId).toInt());
            if (underlinePosV == UnderlineRight) {
                decorationOffsets[DecorationOverline] = QPointF(bottom, 0);
                decorationOffsets[DecorationUnderline] = QPointF(top, 0);
                underlineOverlineFlipped = true;
            } else {
                decorationOffsets[DecorationOverline] = QPointF(top, 0);
                decorationOffsets[DecorationUnderline] = QPointF(bottom, 0);
            }
        }
        decorationOffsets[DecorationLineThrough] = (decorationOffsets.value(DecorationUnderline) + decorationOffsets.value(DecorationOverline)) * 0.5;

        // Now to create a QPainterPath for the given style that stretches
        // over a single decoration rect,
        // transform that and add it to the general paths.
        for (int i = 0; i < decorationRects.size(); i++) {
            QRectF rect = decorationRects.at(i);
            QPainterPath p;
            QPointF pathWidth;
            if (style != Wavy) {
                p.moveTo(QPointF());
                // We're segmenting the path here so it'll be easier to warp
                // when text-on-path is happening.
                if (currentTextPath) {
                    if (isHorizontal) {
                        const qreal total = std::floor(rect.width() / (stroker.width() * 2));
                        const qreal segment = qreal(rect.width() / total);
                        for (int i = 0; i < total; i++) {
                            p.lineTo(p.currentPosition() + QPointF(segment, 0));
                        }
                    } else {
                        const qreal total = std::floor(rect.height() / (stroker.width() * 2));
                        const qreal segment = qreal(rect.height() / total);
                        for (int i = 0; i < total; i++) {
                            p.lineTo(p.currentPosition() + QPointF(0, segment));
                        }
                    }
                } else {
                    if (isHorizontal) {
                        p.lineTo(rect.width(), 0);
                    } else {
                        p.lineTo(0, rect.height());
                    }
                }
            }
            if (style == Double) {
                qreal linewidthOffset = qMax(stroker.width() * 1.5, minimumDecorationThickness * 2);
                if (isHorizontal) {
                    p.addPath(p.translated(0, linewidthOffset));
                    pathWidth = QPointF(0, -linewidthOffset);
                } else {
                    p.addPath(p.translated(linewidthOffset, 0));
                    pathWidth = QPointF(linewidthOffset, 0);
                }

            } else if (style == Wavy) {
                qreal width = isHorizontal ? rect.width() : rect.height();
                qreal height = stroker.width() * 2;

                bool down = true;
                p.moveTo(QPointF());

                for (int i = 0; i < qFloor(width / height); i++) {
                    if (down) {
                        p.lineTo(p.currentPosition().x() + height, height);
                    } else {
                        p.lineTo(p.currentPosition().x() + height, 0);
                    }
                    down = !down;
                }
                qreal offset = fmod(width, height);
                if (down) {
                    p.lineTo(width, offset);
                } else {
                    p.lineTo(width, height - offset);
                }
                pathWidth = QPointF(0, -stroker.width());

                // Rotate for vertical.
                if (!isHorizontal) {
                    for (int i = 0; i < p.elementCount(); i++) {
                        p.setElementPositionAt(i, p.elementAt(i).y - (stroker.width() * 2), p.elementAt(i).x);
                    }
                    pathWidth = QPointF(stroker.width(), 0);
                }
            }

            p.translate(firstPos.at(i).x(), firstPos.at(i).y());
            if (underlineOverlineFlipped) {
                decorationOffsets[DecorationUnderline] += pathWidth;
            } else {
                decorationOffsets[DecorationOverline] += pathWidth;
            }
            decorationOffsets[DecorationLineThrough] += (pathWidth * 0.5);

            Q_FOREACH (TextDecoration type, decorationPaths.keys()) {
                if (decor.testFlag(type)) {
                    QPointF offset = decorationOffsets.value(type);

                    if (currentTextPath) {
                        QPainterPath path = currentTextPath->outline();
                        path = currentTextPath->transformation().map(path);
                        if (textPathSide) {
                            path = path.toReversed();
                        }

                        decorationPaths[type].addPath(
                            stretchGlyphOnPath(p.translated(offset), path, isHorizontal, currentTextPathOffset, currentTextPath->isClosedSubpath(0)));
                    } else {
                        decorationPaths[type].addPath(p.translated(offset));
                    }
                }
            }
        }

        // And finally add the paths to the chunkshape.

        chunkShape->layoutInterface()->clearTextDecorations();

        Q_FOREACH (TextDecoration type, decorationPaths.keys()) {
            QPainterPath decorationPath = decorationPaths.value(type);
            if (!decorationPath.isEmpty()) {
                stroker.setWidth(qMax(minimumDecorationThickness, chunkShape->layoutInterface()->getTextDecorationWidth(type)));
                decorationPath = stroker.createStroke(decorationPath).simplified();
                chunkShape->layoutInterface()->addTextDecoration(type, decorationPath.simplified());
            }
        }
    }
    currentIndex = j;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void KoSvgTextShape::Private::applyAnchoring(QVector<CharacterResult> &result, bool isHorizontal)
{
    int i = 0;
    int start = 0;

    while (start < result.size()) {
        qreal a = 0;
        qreal b = 0;
        for (i = start; i < result.size(); i++) {
            if (!result.at(i).addressable) {
                continue;
            }
            if (result.at(i).anchored_chunk && i > start) {
                break;
            }
            qreal pos = isHorizontal ? result.at(i).finalPosition.x() : result.at(i).finalPosition.y();
            qreal advance = isHorizontal ? result.at(i).advance.x() : result.at(i).advance.y();

            if (result.at(i).anchored_chunk) {
                a = qMin(pos, pos + advance);
                b = qMax(pos, pos + advance);
            } else {
                a = qMin(a, qMin(pos, pos + advance));
                b = qMax(b, qMax(pos, pos + advance));
            }
        }

        const bool rtl = result.at(start).direction == KoSvgText::DirectionRightToLeft;
        qreal shift = isHorizontal ? result.at(start).finalPosition.x() : result.at(start).finalPosition.y();

        if ((result.at(start).anchor == KoSvgText::AnchorStart && !rtl) || (result.at(start).anchor == KoSvgText::AnchorEnd && rtl)) {
            shift = shift - a;

        } else if ((result.at(start).anchor == KoSvgText::AnchorEnd && !rtl) || (result.at(start).anchor == KoSvgText::AnchorStart && rtl)) {
            shift = shift - b;

        } else {
            shift = shift - (a + b) * 0.5;
        }

        const QPointF shiftP = isHorizontal ? QPointF(shift, 0) : QPointF(0, shift);

        for (int j = start; j < i; j++) {
            result[j].finalPosition += shiftP;
        }
        start = i;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
qreal KoSvgTextShape::Private::characterResultOnPath(CharacterResult &cr, qreal length, qreal offset, bool isHorizontal, bool isClosed)
{
    const bool rtl = (cr.direction == KoSvgText::DirectionRightToLeft);
    qreal mid = cr.finalPosition.x() + (cr.advance.x() * 0.5) + offset;
    if (!isHorizontal) {
        mid = cr.finalPosition.y() + (cr.advance.y() * 0.5) + offset;
    }
    if (isClosed) {
        if ((cr.anchor == KoSvgText::AnchorStart && !rtl) || (cr.anchor == KoSvgText::AnchorEnd && rtl)) {
            if (mid - offset < 0 || mid - offset > length) {
                cr.hidden = true;
            }
        } else if ((cr.anchor == KoSvgText::AnchorEnd && !rtl) || (cr.anchor == KoSvgText::AnchorStart && rtl)) {
            if (mid - offset < -length || mid - offset > 0) {
                cr.hidden = true;
            }
        } else {
            if (mid - offset < -(length * 0.5) || mid - offset > (length * 0.5)) {
                cr.hidden = true;
            }
        }
        if (mid < 0) {
            mid += length;
        }
        mid = fmod(mid, length);
    } else {
        if (mid < 0 || mid > length) {
            cr.hidden = true;
        }
    }
    return mid;
}

QPainterPath KoSvgTextShape::Private::stretchGlyphOnPath(const QPainterPath &glyph,
                                                         const QPainterPath &path,
                                                         bool isHorizontal,
                                                         qreal offset,
                                                         bool isClosed)
{
    QPainterPath p = glyph;
    for (int i = 0; i < glyph.elementCount(); i++) {
        qreal mid = isHorizontal ? glyph.elementAt(i).x + offset : glyph.elementAt(i).y + offset;
        qreal midUnbound = mid;
        if (isClosed) {
            if (mid < 0) {
                mid += path.length();
            }
            mid = fmod(mid, qreal(path.length()));
            midUnbound = mid;
        } else {
            mid = qBound(0.0, mid, qreal(path.length()));
        }
        const qreal percent = path.percentAtLength(mid);
        const QPointF pos = path.pointAtPercent(percent);
        qreal tAngle = path.angleAtPercent(percent);
        if (tAngle > 180) {
            tAngle = 0 - (360 - tAngle);
        }
        const QPointF vectorT(qCos(qDegreesToRadians(tAngle)), -qSin(qDegreesToRadians(tAngle)));
        QPointF finalPos = pos;
        if (isHorizontal) {
            QPointF vectorN(-vectorT.y(), vectorT.x());
            const qreal o = mid - (midUnbound);
            finalPos = pos - (o * vectorT) + (glyph.elementAt(i).y * vectorN);
        } else {
            QPointF vectorN(vectorT.y(), -vectorT.x());
            const qreal o = mid - (midUnbound);
            finalPos = pos - (o * vectorT) + (glyph.elementAt(i).x * vectorN);
        }
        p.setElementPositionAt(i, finalPos.x(), finalPos.y());
    }
    return p;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void KoSvgTextShape::Private::applyTextPath(const KoShape *rootShape, QVector<CharacterResult> &result, bool isHorizontal)
{
    // Unlike all the other applying functions, this one only iterrates over the
    // top-level. SVG is not designed to have nested textPaths. Source:
    // https://github.com/w3c/svgwg/issues/580
    const KoSvgTextChunkShape *chunkShape = dynamic_cast<const KoSvgTextChunkShape *>(rootShape);
    KIS_SAFE_ASSERT_RECOVER_RETURN(chunkShape);
    bool inPath = false;
    bool afterPath = false;
    int currentIndex = 0;
    QPointF pathEnd;
    Q_FOREACH (KoShape *child, chunkShape->shapes()) {
        const KoSvgTextChunkShape *textPathChunk = dynamic_cast<const KoSvgTextChunkShape *>(child);
        KIS_SAFE_ASSERT_RECOVER_RETURN(textPathChunk);
        int endIndex = currentIndex + textPathChunk->layoutInterface()->numChars(true);

        KoPathShape *shape = dynamic_cast<KoPathShape *>(textPathChunk->layoutInterface()->textPath());
        if (shape) {
            QPainterPath path = shape->outline();
            path = shape->transformation().map(path);
            inPath = true;
            if (textPathChunk->layoutInterface()->textOnPathInfo().side == KoSvgText::TextPathSideRight) {
                path = path.toReversed();
            }
            qreal length = path.length();
            qreal offset = 0.0;
            bool isClosed = (shape->isClosedSubpath(0) && shape->subpathCount() == 1);
            if (textPathChunk->layoutInterface()->textOnPathInfo().startOffsetIsPercentage) {
                offset = length * (0.01 * textPathChunk->layoutInterface()->textOnPathInfo().startOffset);
            } else {
                offset = textPathChunk->layoutInterface()->textOnPathInfo().startOffset;
            }
            bool stretch = textPathChunk->layoutInterface()->textOnPathInfo().method == KoSvgText::TextPathStretch;

            for (int i = currentIndex; i < endIndex; i++) {
                CharacterResult cr = result[i];

                if (!cr.middle) {
                    const qreal mid = characterResultOnPath(cr, length, offset, isHorizontal, isClosed);
                    if (!cr.hidden) {
                        if (stretch && !cr.path.isEmpty()) {
                            const QTransform tf = cr.finalTransform();
                            QPainterPath glyph = stretchGlyphOnPath(tf.map(cr.path), path, isHorizontal, offset, isClosed);
                            cr.path = glyph;
                        }
                        const qreal percent = path.percentAtLength(mid);
                        const QPointF pos = path.pointAtPercent(percent);
                        qreal tAngle = path.angleAtPercent(percent);
                        if (tAngle > 180) {
                            tAngle = 0 - (360 - tAngle);
                        }
                        const QPointF vectorT(qCos(qDegreesToRadians(tAngle)), -qSin(qDegreesToRadians(tAngle)));
                        if (isHorizontal) {
                            cr.rotate -= qDegreesToRadians(tAngle);
                            QPointF vectorN(-vectorT.y(), vectorT.x());
                            const qreal o = (cr.advance.x() * 0.5);
                            cr.finalPosition = pos - (o * vectorT) + (cr.finalPosition.y() * vectorN);
                        } else {
                            cr.rotate -= qDegreesToRadians(tAngle + 90);
                            QPointF vectorN(vectorT.y(), -vectorT.x());
                            const qreal o = (cr.advance.y() * 0.5);
                            cr.finalPosition = pos - (o * vectorT) + (cr.finalPosition.x() * vectorN);
                        }
                        if (stretch && !cr.path.isEmpty()) {
                            const QTransform tf = cr.finalTransform();
                            cr.path = tf.inverted().map(cr.path);
                        }
                    }
                }
                result[i] = cr;
            }
            pathEnd = path.pointAtPercent(1.0);
        } else {
            if (inPath) {
                inPath = false;
                afterPath = true;
                pathEnd -= result.at(currentIndex).finalPosition;
            }
            if (afterPath) {
                for (int i = currentIndex; i < endIndex; i++) {
                    CharacterResult cr = result[i];
                    if (cr.anchored_chunk) {
                        afterPath = false;
                    } else {
                        cr.finalPosition += pathEnd;
                        result[i] = cr;
                    }
                }
            }
        }
        currentIndex = endIndex;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void KoSvgTextShape::Private::paintPaths(QPainter &painter,
                                         const QPainterPath &rootOutline,
                                         const KoShape *rootShape,
                                         const QVector<CharacterResult> &result,
                                         QPainterPath &chunk,
                                         int &currentIndex)
{
    const KoSvgTextChunkShape *chunkShape = dynamic_cast<const KoSvgTextChunkShape *>(rootShape);
    KIS_SAFE_ASSERT_RECOVER_RETURN(chunkShape);
    QMap<KoSvgText::TextDecoration, QPainterPath> textDecorations = chunkShape->layoutInterface()->textDecorations();
    QColor textDecorationColor = chunkShape->textProperties().propertyOrDefault(KoSvgTextProperties::TextDecorationColorId).value<QColor>();

    if (textDecorations.contains(KoSvgText::DecorationUnderline)) {
        if (chunkShape->background() && !textDecorationColor.isValid() && textDecorationColor != Qt::transparent) {
            chunkShape->background()->paint(painter, textDecorations.value(KoSvgText::DecorationUnderline));
        } else if (textDecorationColor.isValid()) {
            painter.fillPath(textDecorations.value(KoSvgText::DecorationUnderline), textDecorationColor);
        }
        if (chunkShape->stroke()) {
            QScopedPointer<KoShape> shape(KoPathShape::createShapeFromPainterPath(textDecorations.value(KoSvgText::DecorationUnderline)));
            chunkShape->stroke()->paint(shape.data(), painter);
        }
    }
    if (textDecorations.contains(KoSvgText::DecorationOverline)) {
        if (chunkShape->background() && !textDecorationColor.isValid()) {
            chunkShape->background()->paint(painter, textDecorations.value(KoSvgText::DecorationOverline));
        } else if (textDecorationColor.isValid()) {
            painter.fillPath(textDecorations.value(KoSvgText::DecorationOverline), textDecorationColor);
        }
        if (chunkShape->stroke()) {
            QScopedPointer<KoShape> shape(KoPathShape::createShapeFromPainterPath(textDecorations.value(KoSvgText::DecorationOverline)));
            chunkShape->stroke()->paint(shape.data(), painter);
        }
    }

    if (chunkShape->isTextNode()) {
        const int j = currentIndex + chunkShape->layoutInterface()->numChars(true);

        const QRect shapeGlobalClipRect = painter.transform().mapRect(chunkShape->outlineRect()).toAlignedRect();

        if (shapeGlobalClipRect.isValid()) {
            KoClipMaskPainter fillPainter(&painter, shapeGlobalClipRect);
            if (chunkShape->background()) {
                chunkShape->background()->paint(*fillPainter.shapePainter(), rootOutline);
                fillPainter.maskPainter()->fillPath(rootOutline, Qt::black);
                if (textRendering != OptimizeSpeed) {
                    fillPainter.maskPainter()->setRenderHint(QPainter::Antialiasing, true);
                    fillPainter.maskPainter()->setRenderHint(QPainter::SmoothPixmapTransform, true);
                } else {
                    fillPainter.maskPainter()->setRenderHint(QPainter::Antialiasing, false);
                    fillPainter.maskPainter()->setRenderHint(QPainter::SmoothPixmapTransform, false);
                }
            }
            QPainterPath textDecorationsRest;
            textDecorationsRest.setFillRule(Qt::WindingFill);

            for (int i = currentIndex; i < j; i++) {
                if (result.at(i).addressable && !result.at(i).hidden) {
                    const QTransform tf = result.at(i).finalTransform();

                    /**
                     * Make sure the character touches the painter's clip rect,
                     * otherwise we can just skip it
                     */
                    const QRectF boundingRect = tf.mapRect(result.at(i).boundingBox);
                    const QRectF clipRect = painter.clipBoundingRect();
                    if (boundingRect.isEmpty() ||
                        (!clipRect.contains(boundingRect) &&
                         !clipRect.intersects(boundingRect))) continue;

                    /* Debug
                    painter.save();
                    painter.setBrush(Qt::transparent);
                    QPen pen (QColor(0, 0, 0, 50));
                    pen.setWidthF(72./xRes);
                    painter.setPen(pen);
                    painter.drawPolygon(tf.map(result.at(i).path.boundingRect()));
                    QColor penColor = result.at(i).anchored_chunk?
                                             result.at(i).isHanging? Qt::red:
                    Qt::magenta: result.at(i).lineEnd==NoChange? Qt::cyan:
                    Qt::yellow; pen.setColor(penColor);
                    pen.setWidthF(72./xRes);
                    painter.setPen(pen);
                    painter.drawPolygon(tf.map(result.at(i).boundingBox));
                    if (result.at(i).breakType == SoftBreak){
                        painter.setPen(Qt::blue);
                        painter.drawPoint(tf.mapRect(result.at(i).boundingBox).center());
                    }
                    if (result.at(i).breakType == HardBreak){
                        painter.setPen(Qt::red);
                        painter.drawPoint(tf.mapRect(result.at(i).boundingBox).center());
                    }
                    painter.setPen(Qt::red);
                    painter.drawPoint(result.at(i).finalPosition);
                    painter.restore();
                    //*/
                    /**
                     * There's an annoying problem here that officially speaking
                     * the chunks need to be unified into one single path before
                     * drawing, so there's no weirdness with the stroke, but
                     * QPainterPath's union function will frequently lead to
                     * reduced quality of the paths because of 'numerical
                     * instability'.
                     */

                    QPainterPath p = tf.map(result.at(i).path);
                    // if (chunk.intersects(p)) {
                    //     chunk |= tf.map(result.at(i).path);
                    // } else {
                    if (!result.at(i).colorLayers.empty()) {
                        for (int c = 0; c < result.at(i).colorLayers.size(); c++) {
                            QBrush color = result.at(i).colorLayerColors.at(c);
                            bool replace = result.at(i).replaceWithForeGroundColor.at(c);
                            // In theory we can use the pattern or gradient as well
                            // for ColorV0 fonts, but ColorV1 fonts can have
                            // gradients, so I am hesitant.
                            KoColorBackground *b = dynamic_cast<KoColorBackground *>(chunkShape->background().data());
                            if (b && replace) {
                                color = b->brush();
                            }
                            painter.fillPath(tf.map(result.at(i).colorLayers.at(c)), color);
                        }
                    } else {
                        chunk.addPath(p);
                    }
                    //}
                    if (p.isEmpty() && !result.at(i).image.isNull()) {
                        if (result.at(i).image.isGrayscale() || result.at(i).image.format() == QImage::Format_Mono) {
                            fillPainter.maskPainter()->save();
                            fillPainter.maskPainter()->translate(result.at(i).finalPosition.x(), result.at(i).finalPosition.y());
                            fillPainter.maskPainter()->rotate(qRadiansToDegrees(result.at(i).rotate));
                            fillPainter.maskPainter()->setCompositionMode(QPainter::CompositionMode_Plus);
                            fillPainter.maskPainter()->drawImage(result.at(i).boundingBox, result.at(i).image);
                            fillPainter.maskPainter()->restore();
                        } else {
                            painter.save();
                            painter.translate(result.at(i).finalPosition.x(), result.at(i).finalPosition.y());
                            painter.rotate(qRadiansToDegrees(result.at(i).rotate));
                            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                            painter.drawImage(result.at(i).boundingBox, result.at(i).image);
                            painter.restore();
                        }
                    }
                }
            }
            if (chunkShape->background()) {
                chunk.setFillRule(Qt::WindingFill);
                fillPainter.maskPainter()->fillPath(chunk, Qt::white);
            }
            if (!textDecorationsRest.isEmpty()) {
                fillPainter.maskPainter()->fillPath(textDecorationsRest.simplified(), Qt::white);
            }
            fillPainter.renderOnGlobalPainter();

            KoShapeStrokeSP maskStroke;
            if (chunkShape->stroke()) {
                KoShapeStrokeSP stroke = qSharedPointerDynamicCast<KoShapeStroke>(chunkShape->stroke());

                if (stroke) {
                    if (stroke->lineBrush().gradient()) {
                        KoClipMaskPainter strokePainter(&painter, shapeGlobalClipRect);
                        strokePainter.shapePainter()->fillRect(rootOutline.boundingRect(), stroke->lineBrush());
                        maskStroke = KoShapeStrokeSP(new KoShapeStroke(*stroke.data()));
                        maskStroke->setColor(Qt::white);
                        maskStroke->setLineBrush(Qt::white);
                        strokePainter.maskPainter()->fillPath(rootOutline, Qt::black);
                        if (textRendering != OptimizeSpeed) {
                            strokePainter.maskPainter()->setRenderHint(QPainter::Antialiasing, true);
                        } else {
                            strokePainter.maskPainter()->setRenderHint(QPainter::Antialiasing, false);
                        }
                        {
                            QScopedPointer<KoShape> shape(KoPathShape::createShapeFromPainterPath(chunk));
                            maskStroke->paint(shape.data(), *strokePainter.maskPainter());
                        }
                        if (!textDecorationsRest.isEmpty()) {
                            QScopedPointer<KoShape> shape(KoPathShape::createShapeFromPainterPath(textDecorationsRest));
                            maskStroke->paint(shape.data(), *strokePainter.maskPainter());
                        }
                        strokePainter.renderOnGlobalPainter();
                    } else {
                        {
                            QScopedPointer<KoShape> shape(KoPathShape::createShapeFromPainterPath(chunk));
                            stroke->paint(shape.data(), painter);
                        }
                        if (!textDecorationsRest.isEmpty()) {
                            QScopedPointer<KoShape> shape(KoPathShape::createShapeFromPainterPath(textDecorationsRest));
                            stroke->paint(shape.data(), painter);
                        }
                    }
                }
            }
        }
        chunk = QPainterPath();
        currentIndex = j;
    } else {
        Q_FOREACH (KoShape *child, chunkShape->shapes()) {
            /**
             * We pass the root outline to make sure that all gradient and
             * object-size-related decorations are rendered correctly.
             */
            paintPaths(painter, rootOutline, child, result, chunk, currentIndex);
        }
    }
    if (textDecorations.contains(KoSvgText::DecorationLineThrough)) {
        if (chunkShape->background() && !textDecorationColor.isValid() && textDecorationColor != Qt::transparent) {
            chunkShape->background()->paint(painter, textDecorations.value(KoSvgText::DecorationLineThrough));
        } else if (textDecorationColor.isValid()) {
            painter.fillPath(textDecorations.value(KoSvgText::DecorationLineThrough), textDecorationColor);
        }
        if (chunkShape->stroke()) {
            QScopedPointer<KoShape> shape(KoPathShape::createShapeFromPainterPath(textDecorations.value(KoSvgText::DecorationLineThrough)));
            chunkShape->stroke()->paint(shape.data(), painter);
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
QList<KoShape *> KoSvgTextShape::Private::collectPaths(const KoShape *rootShape, QVector<CharacterResult> &result, int &currentIndex)
{
    const KoSvgTextChunkShape *chunkShape = dynamic_cast<const KoSvgTextChunkShape *>(rootShape);

    QList<KoShape *> shapes;

    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(chunkShape, shapes);

    QMap<KoSvgText::TextDecoration, QPainterPath> textDecorations = chunkShape->layoutInterface()->textDecorations();
    QColor textDecorationColor = chunkShape->textProperties().propertyOrDefault(KoSvgTextProperties::TextDecorationColorId).value<QColor>();
    QSharedPointer<KoShapeBackground> decorationColor = chunkShape->background();
    if (textDecorationColor.isValid()) {
        decorationColor = QSharedPointer<KoColorBackground>(new KoColorBackground(textDecorationColor));
    }

    if (textDecorations.contains(KoSvgText::DecorationUnderline)) {
        KoPathShape *shape = KoPathShape::createShapeFromPainterPath(textDecorations.value(KoSvgText::DecorationUnderline));
        shape->setBackground(decorationColor);
        shape->setStroke(chunkShape->stroke());
        shape->setZIndex(chunkShape->zIndex());
        shape->setFillRule(Qt::WindingFill);
        shapes.append(shape);
    }
    if (textDecorations.contains(KoSvgText::DecorationOverline)) {
        KoPathShape *shape = KoPathShape::createShapeFromPainterPath(textDecorations.value(KoSvgText::DecorationOverline));
        shape->setBackground(decorationColor);
        shape->setStroke(chunkShape->stroke());
        shape->setZIndex(chunkShape->zIndex());
        shape->setFillRule(Qt::WindingFill);
        shapes.append(shape);
    }

    if (chunkShape->isTextNode()) {
        QPainterPath chunk;

        const int j = currentIndex + chunkShape->layoutInterface()->numChars(true);
        for (int i = currentIndex; i < j; i++) {
            if (result.at(i).addressable && !result.at(i).hidden) {
                const QTransform tf = result.at(i).finalTransform();
                QPainterPath p = tf.map(result.at(i).path);
                if (!result.at(i).colorLayers.empty()) {
                    for (int c = 0; c < result.at(i).colorLayers.size(); c++) {
                        QBrush color = result.at(i).colorLayerColors.at(c);
                        bool replace = result.at(i).replaceWithForeGroundColor.at(c);
                        // In theory we can use the pattern or gradient as well
                        // for ColorV0 fonts, but ColorV1 fonts can have
                        // gradients, so I am hesitant.
                        KoColorBackground *b = dynamic_cast<KoColorBackground *>(chunkShape->background().data());
                        if (b && replace) {
                            color = b->brush();
                        }
                        KoPathShape *shape = KoPathShape::createShapeFromPainterPath(tf.map(result.at(i).colorLayers.at(c)));
                        shape->setBackground(QSharedPointer<KoColorBackground>(new KoColorBackground(color.color())));
                        shape->setZIndex(chunkShape->zIndex());
                        shape->setFillRule(Qt::WindingFill);
                        shapes.append(shape);
                    }
                } else {
                    chunk.addPath(p);
                }
            }
        }
        KoPathShape *shape = KoPathShape::createShapeFromPainterPath(chunk);
        shape->setBackground(chunkShape->background());
        shape->setStroke(chunkShape->stroke());
        shape->setZIndex(chunkShape->zIndex());
        shape->setFillRule(Qt::WindingFill);
        shapes.append(shape);
        currentIndex = j;

    } else {
        Q_FOREACH (KoShape *child, chunkShape->shapes()) {
            shapes.append(collectPaths(child, result, currentIndex));
        }
    }
    if (textDecorations.contains(KoSvgText::DecorationLineThrough)) {
        KoPathShape *shape = KoPathShape::createShapeFromPainterPath(textDecorations.value(KoSvgText::DecorationLineThrough));
        shape->setBackground(decorationColor);
        shape->setStroke(chunkShape->stroke());
        shape->setZIndex(chunkShape->zIndex());
        shape->setFillRule(Qt::WindingFill);
        shapes.append(shape);
    }
    return shapes;
}

bool KoSvgTextShape::isRootTextNode() const
{
    return true;
}

KoSvgTextShapeFactory::KoSvgTextShapeFactory()
    : KoShapeFactoryBase(KoSvgTextShape_SHAPEID, i18nc("Text label in SVG Text Tool", "Text"))
{
    setToolTip(i18n("SVG Text Shape"));
    setIconName(koIconNameCStr("x-shape-text"));
    setLoadingPriority(5);
    setXmlElementNames(KoXmlNS::svg, QStringList("text"));

    KoShapeTemplate t;
    t.name = i18n("SVG Text");
    t.iconName = koIconName("x-shape-text");
    t.toolTip = i18n("SVG Text Shape");
    addTemplate(t);
}

KoShape *KoSvgTextShapeFactory::createDefaultShape(KoDocumentResourceManager *documentResources) const
{
    debugFlake << "Create default svg text shape";

    KoSvgTextShape *shape = new KoSvgTextShape();
    shape->setShapeId(KoSvgTextShape_SHAPEID);

    KoSvgTextShapeMarkupConverter converter(shape);
    converter.convertFromSvg(i18nc("Default text for the text shape", "<text>Placeholder Text</text>"),
                             "<defs/>",
                             QRectF(0, 0, 200, 60),
                             documentResources->documentResolution());

    debugFlake << converter.errors() << converter.warnings();

    return shape;
}

KoShape *KoSvgTextShapeFactory::createShape(const KoProperties *params, KoDocumentResourceManager *documentResources) const
{
    KoSvgTextShape *shape = new KoSvgTextShape();
    shape->setShapeId(KoSvgTextShape_SHAPEID);

    QString svgText = params->stringProperty("svgText", i18nc("Default text for the text shape", "<text>Placeholder Text</text>"));
    QString defs = params->stringProperty("defs" , "<defs/>");
    QRectF shapeRect = QRectF(0, 0, 200, 60);
    QVariant rect = params->property("shapeRect");

    if (rect.type()==QVariant::RectF) {
        shapeRect = rect.toRectF();
    }

    KoSvgTextShapeMarkupConverter converter(shape);
    converter.convertFromSvg(svgText,
                             defs,
                             shapeRect,
                             documentResources->documentResolution());

    shape->setPosition(shapeRect.topLeft());

    return shape;
}

bool KoSvgTextShapeFactory::supports(const QDomElement &/*e*/, KoShapeLoadingContext &/*context*/) const
{
    return false;
}
