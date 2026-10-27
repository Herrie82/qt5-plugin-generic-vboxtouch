/****************************************************************************
**
** Copyright (C) 2026 Herman van Hazendonk <github.com@herrie.org>
**
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
****************************************************************************/

// Tests for the touch screen handler, on the offscreen platform.
//
// The input devices are FIFOs. ioctl() is defined here, so the plugin's
// calls - compiled into this binary - reach it: for the FIFO paths it
// answers like an evdev tablet, an evdev mouse or vboxguest would; every
// other fd goes to the kernel.

#include "vboxtouch.h"

#include <QtTest>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QTemporaryDir>
#include <QTouchEvent>
#include <QWindow>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QPointingDevice>
#else
#include <QTouchDevice>
#endif

#include <qpa/qwindowsysteminterface.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <string>

namespace {

enum class Kind { AbsTablet, RelMouse, VboxGuest };

std::mutex g_lock;
std::map<std::string, Kind> g_devices;
int g_guestX = 0;
int g_guestY = 0;

const int TabletMax = 32767; // VirtualBox's USB tablet

bool kindOf(int fd, Kind *kind)
{
    char link[64];
    char path[4096];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0)
        return false;
    path[n] = '\0';
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_devices.find(path);
    if (it == g_devices.end())
        return false;
    *kind = it->second;
    return true;
}

// Mirrors the request the plugin sends (vboxtouch.cpp).
struct MouseStatus {
    uint32_t size, version, type;
    int32_t rc;
    uint32_t reserved1, reserved2, features;
    int32_t x, y;
};

int fakeIoctl(Kind kind, unsigned long request, void *arg)
{
    const unsigned type = _IOC_TYPE(request);
    const unsigned nr = _IOC_NR(request);

    if (kind == Kind::VboxGuest) {
        if (type != 'V') {
            errno = ENOTTY;
            return -1;
        }
        if (nr == 3 && _IOC_SIZE(request) == sizeof(MouseStatus)) {
            MouseStatus *status = static_cast<MouseStatus *>(arg);
            std::lock_guard<std::mutex> lock(g_lock);
            status->features = 2; // VBOXMOUSE_IS_ABSOLUTE
            status->x = g_guestX;
            status->y = g_guestY;
            status->rc = 0;
        }
        return 0; // set pointer shape (3), set mouse status (15)
    }

    if (type != 'E') {
        errno = ENOTTY;
        return -1;
    }
    if (request == EVIOCGRAB)
        return 0;
    if (nr == _IOC_NR(EVIOCGBIT(EV_ABS, 0))) {
        memset(arg, 0, _IOC_SIZE(request));
        if (kind == Kind::AbsTablet) {
            unsigned char *bits = static_cast<unsigned char *>(arg);
            bits[ABS_X / 8] |= 1 << (ABS_X % 8);
            bits[ABS_Y / 8] |= 1 << (ABS_Y % 8);
        }
        return 0;
    }
    if (kind == Kind::AbsTablet && (request == EVIOCGABS(ABS_X) || request == EVIOCGABS(ABS_Y))) {
        input_absinfo *info = static_cast<input_absinfo *>(arg);
        memset(info, 0, sizeof(*info));
        info->maximum = TabletMax;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

} // namespace

extern "C" int ioctl(int fd, unsigned long request, ...) noexcept
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    Kind kind;
    if (kindOf(fd, &kind))
        return fakeIoctl(kind, request, arg);
    return static_cast<int>(syscall(SYS_ioctl, fd, request, arg));
}

namespace {

struct Touch {
    QEvent::Type type;
    QPointF pos;
};

class TouchWindow : public QWindow
{
public:
    QList<Touch> touches;

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchUpdate
            || e->type() == QEvent::TouchEnd) {
            const QTouchEvent *te = static_cast<QTouchEvent *>(e);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (!te->points().isEmpty())
                touches << Touch{ e->type(), te->points().first().globalPosition() };
#else
            if (!te->touchPoints().isEmpty())
                touches << Touch{ e->type(), te->touchPoints().first().screenPos() };
#endif
            e->accept();
            return true;
        }
        return QWindow::event(e);
    }
};

// A FIFO standing in for a device; the write end is opened once the plugin
// holds the read end, and kept open so the plugin never sees EOF.
class FakeDevice
{
public:
    FakeDevice(const QString &path, Kind kind) : m_path(path)
    {
        QVERIFY2(mkfifo(qPrintable(path), 0600) == 0, qPrintable(path));
        std::lock_guard<std::mutex> lock(g_lock);
        g_devices[path.toStdString()] = kind;
    }
    ~FakeDevice()
    {
        if (m_fd >= 0)
            close(m_fd);
        std::lock_guard<std::mutex> lock(g_lock);
        g_devices.erase(m_path.toStdString());
    }
    QString path() const { return m_path; }

    void event(int type, int code, int value)
    {
        if (!openWriter())
            return;
        input_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = type;
        ev.code = code;
        ev.value = value;
        QCOMPARE(write(m_fd, &ev, sizeof(ev)), ssize_t(sizeof(ev)));
    }
    void sync() { event(EV_SYN, SYN_REPORT, 0); }
    void absMove(int x, int y) { event(EV_ABS, ABS_X, x); event(EV_ABS, ABS_Y, y); sync(); }
    void button(bool down) { event(EV_KEY, BTN_LEFT, down ? 1 : 0); sync(); }

    void guestPosition(int x, int y)
    {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_guestX = x;
            g_guestY = y;
        }
        if (!openWriter())
            return;
        const char c = 1;
        QCOMPARE(write(m_fd, &c, 1), ssize_t(1));
    }

private:
    bool openWriter()
    {
        if (m_fd < 0)
            m_fd = open(qPrintable(m_path), O_WRONLY | O_NONBLOCK);
        if (m_fd < 0)
            QTest::qFail(qPrintable("nobody has " + m_path + " open"), __FILE__, __LINE__);
        return m_fd >= 0;
    }

    QString m_path;
    int m_fd = -1;
};

} // namespace

#define COMPARE_POINT(actual, expected) \
    do { \
        const QPointF a_ = (actual); \
        const QPointF e_ = (expected); \
        if (qAbs(a_.x() - e_.x()) > 1.0 || qAbs(a_.y() - e_.y()) > 1.0) \
            QFAIL(qPrintable(QString::fromLatin1("touch at %1,%2, expected %3,%4") \
                             .arg(a_.x()).arg(a_.y()).arg(e_.x()).arg(e_.y()))); \
    } while (0)

class tst_VboxTouch : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void evdevTabletPressMoveRelease();
    void evdevFirstPressIsWhereThePointerIs();
    void evdevRelativeMouseOnlyAddsButtons();
    void evdevDragWithTheButtonOnAnotherDevice();
    void evdevSkipsDevicesThatDoNotOpen();
    void evdevDevicesFromEnvironment();
    void evdevWithoutDevicesDoesNothing();
    void focusOnAWindowThatIsNotQuick();
    void vboxGuestPositionWithEvdevButtons();
    void vboxGuestThatDoesNotOpen();

private:
    QString path(const char *name) const { return m_dir->path() + QLatin1Char('/') + QLatin1String(name); }

    // Runs the event loop until n touches arrived, or for the whole timeout.
    bool waitForTouches(int n, int timeoutMs = 2000)
    {
        QElapsedTimer timer;
        timer.start();
        while (m_window->touches.size() < n && timer.elapsed() < timeoutMs)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return m_window->touches.size() >= n;
    }
    void settle(int ms = 200) { waitForTouches(1 << 30, ms); }
    int touchCount() const { return int(m_window->touches.size()); }
    const Touch &touch(int i) const { return m_window->touches.at(i); }

    // Where a device position (0..range) lands on the screen.
    static QPointF expected(int x, int y, int range)
    {
        const QRect s = QGuiApplication::primaryScreen()->geometry();
        const qreal nx = qreal(qRound(qreal(x) * 65535 / range)) / 65535;
        const qreal ny = qreal(qRound(qreal(y) * 65535 / range)) / 65535;
        return QPointF(nx * (s.width() - 1) + s.x(), ny * (s.height() - 1) + s.y());
    }

    QScopedPointer<QTemporaryDir> m_dir;
    QScopedPointer<TouchWindow> m_window;
};

void tst_VboxTouch::init()
{
    m_dir.reset(new QTemporaryDir(QStringLiteral(TEST_TMPDIR "/tst-XXXXXX")));
    QVERIFY(m_dir->isValid());
    qunsetenv("VIRTUALBOX_TOUCH_EVDEV_MOUSE");
    qunsetenv("VIRTUALBOX_TOUCH_GUEST_DEVICE");

    m_window.reset(new TouchWindow);
    // Frameless: offscreen windows otherwise get a frame, and touches on the
    // screen's edges would land in it.
    m_window->setFlags(Qt::FramelessWindowHint);
    m_window->setGeometry(QGuiApplication::primaryScreen()->geometry());
    m_window->show();
    m_window->requestActivate();
    settle();
    m_window->touches.clear();
}

void tst_VboxTouch::cleanup()
{
    // A test that stopped half way leaves a touch open in Qt, and the next
    // test's press would arrive as an update of it.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QList<const QInputDevice *> devices = QInputDevice::devices();
    for (const QInputDevice *device : devices) {
        if (device->type() == QInputDevice::DeviceType::TouchScreen)
            QWindowSystemInterface::handleTouchCancelEvent(
                nullptr, static_cast<const QPointingDevice *>(device));
    }
#else
    const QList<const QTouchDevice *> devices = QTouchDevice::devices();
    for (const QTouchDevice *device : devices) {
        if (device->type() == QTouchDevice::TouchScreen)
            QWindowSystemInterface::handleTouchCancelEvent(
                nullptr, const_cast<QTouchDevice *>(device));
    }
#endif
    settle(50);
    m_window.reset();
    m_dir.reset();
}

void tst_VboxTouch::evdevTabletPressMoveRelease()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path());

    tablet.absMove(16384, 16384);
    settle();
    QVERIFY2(m_window->touches.isEmpty(), "moving with the button up is not a touch");

    tablet.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(16384, 16384, TabletMax));

    tablet.absMove(8192, 24576);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(8192, 24576, TabletMax));

    tablet.button(false);
    QVERIFY(waitForTouches(3));
    QCOMPARE(touch(2).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(2).pos, expected(8192, 24576, TabletMax));
}

void tst_VboxTouch::evdevFirstPressIsWhereThePointerIs()
{
    // The very first press after start must not land at 0,0.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path());

    tablet.event(EV_ABS, ABS_X, 30000);
    tablet.event(EV_ABS, ABS_Y, 20000);
    tablet.event(EV_KEY, BTN_LEFT, 1);
    tablet.sync();
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(30000, 20000, TabletMax));
}

void tst_VboxTouch::evdevRelativeMouseOnlyAddsButtons()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path() + ":evdev=" + mouse.path());

    tablet.absMove(8192, 8192);
    settle();
    mouse.event(EV_REL, REL_X, 300);
    mouse.event(EV_REL, REL_Y, 300);
    mouse.sync();
    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(8192, 8192, TabletMax));

    mouse.event(EV_REL, REL_X, 50);
    mouse.sync();
    settle();
    QCOMPARE(touchCount(), 1); // relative motion does not move the touch

    mouse.button(false);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(1).pos, expected(8192, 8192, TabletMax));
}

void tst_VboxTouch::evdevDragWithTheButtonOnAnotherDevice()
{
    // With the guest additions running, VirtualBox reports the position on
    // its mouse integration device and the buttons on the PS/2 mouse. A move
    // on the one must not end the touch the other holds.
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path() + ":evdev=" + mouse.path());

    tablet.absMove(24576, 16384);
    settle();
    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);

    tablet.absMove(16384, 16384);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(16384, 16384, TabletMax));

    tablet.absMove(8192, 16384);
    QVERIFY(waitForTouches(3));
    QCOMPARE(touch(2).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(2).pos, expected(8192, 16384, TabletMax));

    mouse.button(false);
    QVERIFY(waitForTouches(4));
    QCOMPARE(touch(3).type, QEvent::TouchEnd);
    COMPARE_POINT(touch(3).pos, expected(8192, 16384, TabletMax));
}

void tst_VboxTouch::evdevSkipsDevicesThatDoNotOpen()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + path("missing") + ":evdev=" + tablet.path());

    tablet.absMove(100, 200);
    tablet.button(true);
    QVERIFY(waitForTouches(1));
    COMPARE_POINT(touch(0).pos, expected(100, 200, TabletMax));
}

void tst_VboxTouch::evdevDevicesFromEnvironment()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    qputenv("VIRTUALBOX_TOUCH_EVDEV_MOUSE", (path("missing") + ":" + tablet.path()).toLocal8Bit());
    VirtualboxTouchScreenHandler handler("mode=evdev");

    tablet.absMove(32767, 0);
    tablet.button(true);
    QVERIFY(waitForTouches(1));
    COMPARE_POINT(touch(0).pos, expected(32767, 0, TabletMax));
}

void tst_VboxTouch::evdevWithoutDevicesDoesNothing()
{
    {
        VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + path("missing"));
        settle();
    }
    QVERIFY(m_window->touches.isEmpty());
}

void tst_VboxTouch::focusOnAWindowThatIsNotQuick()
{
    FakeDevice tablet(path("tablet"), Kind::AbsTablet);
    VirtualboxTouchScreenHandler handler("mode=evdev:evdev=" + tablet.path());

    // Focus moves to a plain QWindow: the zoom indicator must stay out of it.
    QWindow other;
    other.setGeometry(0, 0, 10, 10);
    other.show();
    other.requestActivate();
    settle();
    m_window->requestActivate();
    settle();
}

void tst_VboxTouch::vboxGuestPositionWithEvdevButtons()
{
    FakeDevice guest(path("vboxguest"), Kind::VboxGuest);
    FakeDevice mouse(path("mouse"), Kind::RelMouse);
    VirtualboxTouchScreenHandler handler("vboxguest=" + guest.path() + ":evdev=" + mouse.path());

    // The guest position is 0..65535; the screen is Qt's own geometry when
    // /dev/fb0 cannot be read, which is the case for a normal user.
    guest.guestPosition(16384, 49152);
    settle();
    QVERIFY(m_window->touches.isEmpty());

    mouse.button(true);
    QVERIFY(waitForTouches(1));
    QCOMPARE(touch(0).type, QEvent::TouchBegin);
    COMPARE_POINT(touch(0).pos, expected(16384, 49152, 65535));

    guest.guestPosition(32768, 32768);
    QVERIFY(waitForTouches(2));
    QCOMPARE(touch(1).type, QEvent::TouchUpdate);
    COMPARE_POINT(touch(1).pos, expected(32768, 32768, 65535));

    mouse.button(false);
    QVERIFY(waitForTouches(3));
    QCOMPARE(touch(2).type, QEvent::TouchEnd);
}

void tst_VboxTouch::vboxGuestThatDoesNotOpen()
{
    // Used to delete an uninitialised evdev handler pointer on destruction.
    { VirtualboxTouchScreenHandler handler("vboxguest=" + path("missing")); }
    { VirtualboxTouchScreenHandler handler("mode=bogus:vboxguest=" + path("missing")); }
}

int main(int argc, char **argv)
{
    // Touches go to a window, so a platform is needed; offscreen works
    // without a display and gives every run the same screen.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    tst_VboxTouch test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_vboxtouch.moc"
