#ifndef SPEED_GAUGE_H
#define SPEED_GAUGE_H

#include <QWidget>
#include <QPainter>
#include <QtMath>

class SpeedGauge : public QWidget
{
    Q_OBJECT
public:
    explicit SpeedGauge(QWidget *parent = nullptr)
        : QWidget(parent), m_speed(0), m_max(120) {
        setMinimumSize(280, 220);
    }

    void setSpeed(float kmh) { m_speed = kmh; update(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        int w = width(), h = height();
        int cx = w/2, cy = h-30, r = qMin(w/2-10, h-30);

        /* 背景弧 */
        QPen pen(QColor("#333"), 18, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.drawArc(cx-r, cy-r, 2*r, 2*r, 45*16, 270*16);

        /* 彩色弧段 */
        int seg = 270/4;
        QColor colors[] = {QColor("#4CAF50"), QColor("#FF9800"), QColor("#FF5722"), QColor("#F44336")};
        for (int i=0; i<4; i++) {
            pen.setColor(colors[i]); pen.setWidth(18);
            p.setPen(pen);
            int span = (i==3) ? 270-seg*3 : seg;
            p.drawArc(cx-r, cy-r, 2*r, 2*r, (45+i*seg)*16, span*16);
        }

        /* 刻度线 */
        p.setPen(QPen(Qt::white, 2));
        for (int i=0; i<=12; i++) {
            double ang = (225 - i*22.5) * M_PI / 180.0;
            int x1 = cx + (r-24)*cos(ang), y1 = cy - (r-24)*sin(ang);
            int x2 = cx + (r-10)*cos(ang), y2 = cy - (r-10)*sin(ang);
            p.drawLine(x1, y1, x2, y2);
        }

        /* 指针 */
        double ang = (225 - m_speed/m_max*270) * M_PI / 180.0;
        int nx = cx + (r-40)*cos(ang), ny = cy - (r-40)*sin(ang);
        p.setPen(QPen(QColor("#F44336"), 3));
        p.drawLine(cx, cy, nx, ny);
        p.setBrush(QColor("#F44336"));
        p.drawEllipse(cx-6, cy-6, 12, 12);

        /* 数字 */
        p.setPen(Qt::white);
        QFont f("Arial", 28, QFont::Bold); p.setFont(f);
        p.drawText(QRect(cx-80, cy-r+30, 160, 50), Qt::AlignCenter, QString("%1").arg(m_speed, 0, 'f', 0));
        f.setPointSize(12); p.setFont(f);
        p.drawText(QRect(cx-80, cy-r+65, 160, 20), Qt::AlignCenter, "km/h");
    }

private:
    float m_speed, m_max;
};

#endif
