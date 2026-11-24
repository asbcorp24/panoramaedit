#include <vector>
#include "SphericalAI.h"
#include "SphericalRaster.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;

int wrapped(int x, int width) { return (x % width + width) % width; }
double clamp1(double x) { return std::clamp(x, -1.0, 1.0); }

void addWrappedRow(QRegion &region, int first, int last, int y, int width) {
    const int start = wrapped(first, width);
    const int count = last - first + 1;
    if (count >= width) region += QRect(0, y, width, 1);
    else if (start + count <= width) region += QRect(start, y, count, 1);
    else {
        region += QRect(start, y, width - start, 1);
        region += QRect(0, y, start + count - width, 1);
    }
}

int polarX(int x, int &y, int width, int height) {
    // Crossing a pole changes longitude by 180 degrees. This keeps padded
    // context useful even for masks painted at the very top/bottom rows.
    while (y < 0 || y >= height) {
        if (y < 0) y = -y - 1;
        else y = 2 * height - y - 1;
        x += width / 2;
    }
    return wrapped(x, width);
}
} // namespace

namespace SphericalAI {
QRegion paintMask(QImage &mask, const QVector3D &center, double radiusDegrees) {
    if (mask.isNull() || mask.format() != QImage::Format_Grayscale8 ||
        center.lengthSquared() < 1e-12f)
        return {};
    const int width = mask.width(), height = mask.height();
    const QVector3D anchor = center.normalized();
    const double radius = std::clamp(radiusDegrees, 0.1, 30.0) * pi / 180.0;
    const double latitude = std::asin(clamp1(anchor.y()));
    const double longitude = std::atan2(anchor.x(), -anchor.z());
    const int cy = std::clamp(int((0.5 - latitude / pi) * height), 0, height - 1);
    const int ry = int(std::ceil(radius * height / pi)) + 2;
    QRegion dirty;

    for (int y = std::max(0, cy - ry); y <= std::min(height - 1, cy + ry); ++y) {
        const double phi = pi * (0.5 - (y + 0.5) / height);
        const double denom = std::cos(phi) * std::cos(latitude);
        const double numerator = std::cos(radius) - std::sin(phi) * std::sin(latitude);
        const double threshold = denom > 1e-8 ? numerator / denom
            : (numerator <= 0.0 ? -1.0 : 2.0);
        if (threshold > 1.0) continue;
        const double span = threshold <= -1.0 ? pi : std::acos(clamp1(threshold));
        const int cx = int((longitude / (2 * pi) + 0.5) * width);
        const int rx = int(std::ceil(span * width / (2 * pi))) + 2;
        const bool full = rx >= width / 2;
        const int first = full ? 0 : cx - rx;
        const int last = full ? width - 1 : cx + rx;
        int firstChanged = 0, lastChanged = -1;
        bool changed = false;
        uchar *row = mask.scanLine(y);

        for (int xi = first; xi <= last; ++xi) {
            const int x = wrapped(xi, width);
            const QVector3D direction = SphericalRaster::pixelDirection(x, y, width, height);
            const double angle = std::acos(clamp1(QVector3D::dotProduct(direction, anchor)));
            if (angle >= radius) continue;
            const double softness = std::clamp((radius - angle) / (radius * 0.18), 0.0, 1.0);
            const int next = int(std::lround(softness * 255));
            if (next <= row[x]) continue;
            row[x] = uchar(next);
            if (!changed) firstChanged = xi;
            lastChanged = xi;
            changed = true;
        }
        if (changed) addWrappedRow(dirty, firstChanged, lastChanged, y, width);
    }
    return dirty;
}

std::optional<Patch> preparePatch(const QImage &source, const QImage &mask,
                                  int padding, int maxDimension) {
    if (source.isNull() || mask.isNull() || source.size() != mask.size() ||
        source.format() != QImage::Format_RGBA8888 ||
        mask.format() != QImage::Format_Grayscale8)
        return std::nullopt;

    const int width = source.width(), height = source.height();
    std::vector<bool> occupied(width, false);
    int minY = height, maxY = -1;
    for (int y = 0; y < height; ++y) {
        const uchar *row = mask.constScanLine(y);
        bool any = false;
        for (int x = 0; x < width; ++x) {
            if (row[x] == 0) continue;
            occupied[x] = true;
            any = true;
        }
        if (any) { minY = std::min(minY, y); maxY = y; }
    }
    if (maxY < 0) return std::nullopt;

    // Choose the smallest circular longitude arc covering the selection.
    // A mask straddling the 0°/360° seam therefore produces a small crop.
    int longestGap = 0, gapEnd = -1, currentGap = 0;
    for (int i = 0; i < 2 * width; ++i) {
        if (!occupied[i % width]) {
            currentGap = std::min(currentGap + 1, width);
            if (currentGap > longestGap) {
                longestGap = currentGap;
                gapEnd = i;
            }
        } else currentGap = 0;
    }
    const int arcStart = longestGap ? (gapEnd + 1) % width : 0;
    const int arcWidth = width - longestGap;
    padding = std::clamp(padding, 0, 128);
    if (arcWidth + 2 * padding > maxDimension ||
        maxY - minY + 1 + 2 * padding > maxDimension)
        return std::nullopt;

    Patch patch;
    patch.origin = QPoint(arcStart - padding, minY - padding);
    patch.image = QImage(arcWidth + 2 * padding,
                         maxY - minY + 1 + 2 * padding, QImage::Format_RGBA8888);
    patch.mask = QImage(patch.image.size(), QImage::Format_Grayscale8);
    patch.mask.fill(0);
    for (int py = 0; py < patch.image.height(); ++py) {
        uchar *dst = patch.image.scanLine(py);
        uchar *dstMask = patch.mask.scanLine(py);
        const int actualY = patch.origin.y() + py;
        for (int px = 0; px < patch.image.width(); ++px) {
            const int actualX = patch.origin.x() + px;
            int sampleY = actualY;
            const int sampleX = polarX(actualX, sampleY, width, height);
            const uchar *src = source.constScanLine(sampleY) + sampleX * 4;
            std::copy_n(src, 4, dst + px * 4);
            if (actualY >= 0 && actualY < height)
                dstMask[px] = mask.constScanLine(actualY)[wrapped(actualX, width)];
        }
    }
    return patch;
}

QRegion applyPatch(QImage &layer, QImage &composite, const Patch &patch,
                   const QImage &output, double opacity) {
    if (layer.isNull() || composite.isNull() || layer.size() != composite.size() ||
        patch.image.isNull() || patch.mask.isNull() ||
        output.size() != patch.image.size() ||
        output.format() != QImage::Format_RGBA8888)
        return {};
    opacity = std::clamp(opacity, 0.0, 1.0);
    const int width = layer.width(), height = layer.height();
    QRegion dirty;
    for (int py = 0; py < output.height(); ++py) {
        const int y = patch.origin.y() + py;
        if (y < 0 || y >= height) continue;
        const uchar *maskRow = patch.mask.constScanLine(py);
        const uchar *resultRow = output.constScanLine(py);
        uchar *layerRow = layer.scanLine(y);
        uchar *visibleRow = composite.scanLine(y);
        int firstChanged = 0, lastChanged = -1;
        bool changed = false;
        for (int px = 0; px < output.width(); ++px) {
            const double weight = (maskRow[px] / 255.0) * opacity;
            if (weight <= 0.0) continue;
            const int xUnwrapped = patch.origin.x() + px;
            const int x = wrapped(xUnwrapped, width);
            uchar *visible = visibleRow + 4 * x;
            uchar *edited = layerRow + 4 * x;
            const uchar *result = resultRow + 4 * px;
            bool different = false;
            uchar next[3];
            for (int c = 0; c < 3; ++c) {
                next[c] = uchar(std::clamp(int(std::lround(visible[c] * (1 - weight) +
                    result[c] * weight)), 0, 255));
                different |= next[c] != visible[c];
            }
            if (!different) continue;
            for (int c = 0; c < 3; ++c) visible[c] = edited[c] = next[c];
            visible[3] = edited[3] = 255;
            if (!changed) firstChanged = xUnwrapped;
            lastChanged = xUnwrapped;
            changed = true;
        }
        if (changed) addWrappedRow(dirty, firstChanged, lastChanged, y, width);
    }
    return dirty;
}

QRegion smoothSeam(QImage &layer, QImage &composite, int halfWidthPixels) {
    if (layer.isNull() || composite.isNull() || layer.size() != composite.size() ||
        layer.format() != QImage::Format_RGBA8888 ||
        composite.format() != QImage::Format_RGBA8888) return {};
    const int width = composite.width();
    const int half = std::clamp(halfWidthPixels, 1, std::max(1, width / 4));
    QRegion dirty;
    for (int y = 0; y < composite.height(); ++y) {
        uchar *layerRow = layer.scanLine(y);
        uchar *outRow = composite.scanLine(y);
        bool changed = false;
        for (int d = 0; d < half; ++d) {
            uchar *left = outRow + d * 4;
            uchar *right = outRow + (width - d - 1) * 4;
            uchar *layerLeft = layerRow + d * 4;
            uchar *layerRight = layerRow + (width - d - 1) * 4;
            const double k = std::pow(1.0 - double(d) / half, 2.0);
            bool pairChanged = false;
            for (int c = 0; c < 3; ++c) {
                const double mean = (double(left[c]) + right[c]) * 0.5;
                const uchar l = uchar(std::lround(left[c] * (1 - k) + mean * k));
                const uchar r = uchar(std::lround(right[c] * (1 - k) + mean * k));
                if (l == left[c] && r == right[c]) continue;
                left[c] = l;
                right[c] = r;
                pairChanged = true;
            }
            if (pairChanged) {
                for (int c = 0; c < 3; ++c) {
                    layerLeft[c] = left[c];
                    layerRight[c] = right[c];
                }
                left[3] = right[3] = layerLeft[3] = layerRight[3] = 255;
                changed = true;
            }
        }
        if (changed) {
            dirty += QRect(0, y, half, 1);
            dirty += QRect(width - half, y, half, 1);
        }
    }
    return dirty;
}
QRegion smartSeam(QImage &layer,QImage &composite,int halfWidthPixels,double strength) {
    if(composite.isNull()||layer.size()!=composite.size()||composite.width()<8||composite.height()<8||strength<=0)return {};
    const QImage src=composite;
    const int w=src.width(),h=src.height(),half=std::clamp(halfWidthPixels,2,w/4);
    const int search=std::min({half/2,h/20,48});
    const int step=32, blocks=(h+step-1)/step;
    std::vector<float> shifts(blocks,0);
    auto gray=[&](int x,int y){return float(qGray(src.pixel(x,std::clamp(y,0,h-1))));};
    auto grad=[&](int x,int y){return gray(x,y+1)-gray(x,y-1);};
    for(int b=0;b<blocks;++b){
        int cy=std::min(h-1,b*step+step/2);
        double best=1e30,zero=0;int shift=0;
        for(int d=-search;d<=search;++d){
            double cost=0,energy=0;
            for(int j=-24;j<=24;j+=2){
                float a=grad(0,cy+j),z=grad(w-1,cy+j+d);
                cost+=std::abs(a-z);energy+=std::abs(a)+std::abs(z);
            }
            cost=cost/(energy+50)+0.002*std::abs(d);
            if(d==0)zero=cost;
            if(cost<best){best=cost;shift=d;}
        }
        if(best+0.08<zero)shifts[b]=float(shift);
    }
    auto smooth=shifts;
    for(int b=0;b<blocks;++b)smooth[b]=(shifts[std::max(0,b-1)]+2*shifts[b]+shifts[std::min(blocks-1,b+1)])/4;
    auto sample=[&](int x,float y){
        y=std::clamp(y,0.f,float(h-1));int i=int(y);float f=y-i;
        auto a=src.pixelColor(x,i),b=src.pixelColor(x,std::min(i+1,h-1));
        return QColor::fromRgbF(a.redF()*(1-f)+b.redF()*f,a.greenF()*(1-f)+b.greenF()*f,a.blueF()*(1-f)+b.blueF()*f);
    };
    for(int y=0;y<h;++y){
        float pos=std::clamp(float(y)/step-0.5f,0.f,float(blocks-1));int b=int(pos);
        float shift=(smooth[b]*(1-(pos-b))+smooth[std::min(b+1,blocks-1)]*(pos-b))*float(strength);
        shift*=std::min(1.f,float(std::min(y,h-1-y))/std::max(1,search));
        auto l=sample(0,y-shift/2),r=sample(w-1,y+shift/2);
        float delta[3]={float(r.redF()-l.redF()),float(r.greenF()-l.greenF()),float(r.blueF()-l.blueF())};
        for(int d=0;d<half;++d){
            float t=1.f-float(d)/half;float k=t*t*(3-2*t);
            for(int side=0;side<2;++side){
                int x=side?w-1-d:d;float sign=side?1.f:-1.f;
                auto c=sample(x,y+sign*shift*k/2);
                auto out=QColor::fromRgbF(std::clamp(float(c.redF())-sign*delta[0]*k*float(strength)/2,0.f,1.f),std::clamp(float(c.greenF())-sign*delta[1]*k*float(strength)/2,0.f,1.f),std::clamp(float(c.blueF())-sign*delta[2]*k*float(strength)/2,0.f,1.f));
                layer.setPixelColor(x,y,out);composite.setPixelColor(x,y,out);
            }
        }
    }
    return QRegion(0,0,half,h)+QRegion(w-half,0,half,h);
}
} // namespace SphericalAI
