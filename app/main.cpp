#include "window.h"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

namespace {

QIcon applicationIcon()
{
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 64.0, size / 64.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#E3EBE0"));
        painter.drawRoundedRect(QRectF(2, 2, 60, 60), 15, 15);
        painter.setPen(QPen(QColor("#365C45"), 3.5, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(QPolygonF{{17, 26}, {32, 18}, {47, 26}, {32, 34}});
        painter.drawPolyline(QPolygonF{{17, 34}, {32, 42}, {47, 34}});
        painter.drawPolyline(QPolygonF{{17, 42}, {32, 50}, {47, 42}});
        painter.end();
        icon.addPixmap(image);
    }
    return icon;
}

}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Live2D Recovery"));
    app.setOrganizationName(QStringLiteral("IndependentRecovery"));
    app.setStyle(QStringLiteral("Fusion"));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    app.setWindowIcon(applicationIcon());

    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#F5F5F1"));
    palette.setColor(QPalette::WindowText, QColor("#26342B"));
    palette.setColor(QPalette::Base, QColor("#FFFFFF"));
    palette.setColor(QPalette::AlternateBase, QColor("#F1F3EE"));
    palette.setColor(QPalette::Text, QColor("#26342B"));
    palette.setColor(QPalette::Button, QColor("#FFFFFF"));
    palette.setColor(QPalette::ButtonText, QColor("#26342B"));
    palette.setColor(QPalette::Highlight, QColor("#365C45"));
    palette.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#8B938B"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#8B938B"));
    app.setPalette(palette);

    RecoveryWindow window;
    window.show();
    return app.exec();
}
