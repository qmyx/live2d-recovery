#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QVector>

namespace {
void little16(QByteArray& bytes, quint16 value) {
    bytes.append(char(value)); bytes.append(char(value >> 8));
}
void little32(QByteArray& bytes, quint32 value) {
    little16(bytes, quint16(value)); little16(bytes, quint16(value >> 16));
}

QByteArray iconImage(int size) {
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size / 64.0, size / 64.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#E3EBE0"));
    painter.drawRoundedRect(QRectF(2, 2, 60, 60), 15, 15);
    painter.setPen(QPen(QColor("#365C45"), 3.5, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(QPolygonF{{17, 26}, {32, 18}, {47, 26}, {32, 34}});
    painter.drawPolyline(QPolygonF{{17, 34}, {32, 42}, {47, 34}});
    painter.drawPolyline(QPolygonF{{17, 42}, {32, 50}, {47, 42}});
    painter.end();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return {};
    return png;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) return 2;
    const QVector<int> sizes{16, 24, 32, 48, 64, 128, 256};
    QVector<QByteArray> images;
    for (int size : sizes) {
        images.append(iconImage(size));
        if (images.last().isEmpty()) return 3;
    }
    QByteArray icon;
    little16(icon, 0); little16(icon, 1); little16(icon, quint16(images.size()));
    quint32 offset = 6 + 16 * quint32(images.size());
    for (int index = 0; index < images.size(); ++index) {
        icon.append(char(sizes[index] == 256 ? 0 : sizes[index]));
        icon.append(char(sizes[index] == 256 ? 0 : sizes[index]));
        icon.append(char(0)); icon.append(char(0));
        little16(icon, 1); little16(icon, 32);
        little32(icon, quint32(images[index].size())); little32(icon, offset);
        offset += quint32(images[index].size());
    }
    for (const auto& image : images) icon.append(image);
    QFile output(app.arguments().at(1));
    if (!output.open(QIODevice::WriteOnly)) return 4;
    return output.write(icon) == icon.size() ? 0 : 5;
}
