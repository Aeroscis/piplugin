/*
 * piplugin - Host kit L1: Qt 嵌入区域（实现）
 *
 * 纪律见 pi_host_embed_area.h：只做 attach / resize 转发 / idle 驱动，
 * 不创建顶层窗口、不决定布局、不做视觉装饰。
 */
#include "pi_host_embed_area.h"

#include <QResizeEvent>
#include <QTimer>

/* Qt 的原生窗口句柄 -> 框架的 PiNativeWindow。
 * Windows 上 PiNativeWindow 是 void*，Linux 上是整型，故经 quintptr 转一次。 */
static PiNativeWindow NativeOf(QWidget* widget)
{
    return reinterpret_cast<PiNativeWindow>(static_cast<quintptr>(widget->winId()));
}

PiPluginEmbedArea::PiPluginEmbedArea(QWidget* parent)
    : QWidget(parent)
    , m_session(nullptr)
    , m_slot(PI_PLUGIN_HOST_SESSION_INVALID_SLOT)
    , m_idleTimer(nullptr)
{
    /* 插件要嵌的是一个真正的原生窗口（宿主摆位，本类只负责让它可被嵌入） */
    setAttribute(Qt::WA_NativeWindow);
    /* 刻意不设置样式表 / 不覆写 paintEvent：视觉决策归宿主，QSS 全穿透 */
}

PiResult PiPluginEmbedArea::attach(PiPluginHostSession* session, uint32_t slot, char const* view_id,
                                   bool set_visible)
{
    detachBinding();

    if (!session || !view_id || !view_id[0])
    {
        return PI_E_INVALIDARG;
    }
    if (!pi_plugin_host_session_is_loaded(session, slot))
    {
        return PI_E_INVALIDARG;
    }
    /* 没有界面的插件（headless）不是错误，只是没什么可嵌 */
    if (!pi_plugin_host_session_get_view_provider(session, slot))
    {
        return PI_E_NOINTERFACE;
    }
    /* 先按 id 确认这个视图真的存在：attach_view 失败也能给出准确的码 */
    if (!pi_plugin_host_session_get_view(session, slot, view_id))
    {
        return PI_E_NOTFOUND;
    }

    /* winId() 会先确保本控件拿到原生窗口，再由 session 完成 attach 记账 */
    PiResult hr = pi_plugin_host_session_attach_view(session, slot, view_id, NativeOf(this),
                                                    set_visible ? 1 : 0);
    if (PI_FAILED(hr))
    {
        return hr;
    }

    m_session = session;
    m_slot    = slot;
    m_view_id = view_id;
    return PI_OK;
}

void PiPluginEmbedArea::detachBinding()
{
    m_session = nullptr;
    m_slot    = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
    m_view_id.clear();
}

IPiPluginView* PiPluginEmbedArea::view() const
{
    if (!m_session || m_view_id.empty())
    {
        return nullptr;
    }
    return pi_plugin_host_session_get_view(m_session, m_slot, m_view_id.c_str());
}

IPiPluginView* PiPluginEmbedArea::view(char const* view_id) const
{
    if (!m_session || !view_id || !view_id[0])
    {
        return nullptr;
    }
    return pi_plugin_host_session_get_view(m_session, m_slot, view_id);
}

std::string const& PiPluginEmbedArea::boundViewId() const
{
    return m_view_id;
}

void PiPluginEmbedArea::driveIdle()
{
    IPiPluginView* v = view();
    if (v)
    {
        pi_plugin_view_on_idle(v);
    }
}

void PiPluginEmbedArea::setAutoIdleEnabled(bool enabled)
{
    if (enabled)
    {
        if (!m_idleTimer)
        {
            m_idleTimer = new QTimer(this);
            m_idleTimer->setInterval(0); /* 每次事件循环迭代一次 */
            connect(m_idleTimer, &QTimer::timeout, this, [this]()
                    { driveIdle(); });
        }
        m_idleTimer->start();
    }
    else if (m_idleTimer)
    {
        m_idleTimer->stop();
    }
}

void PiPluginEmbedArea::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    /* 尺寸由宿主的布局决定，本类只负责转发 */
    IPiPluginView* v = view();
    if (v && width() > 0 && height() > 0)
    {
        pi_plugin_view_on_resize(v, width(), height());
    }
}
