/*
 * piplugin - Host kit L1: Qt 嵌入区域 (piplugin_host_qt)
 *
 * L1 的职责（见 host_kits/README.md「三层结构」）：把**宿主自己创建的**容器变成 embed host。
 * 本类只做三件机制：
 *   1. attach    —— 把宿主指定的插件 view 嵌进本控件的原生窗口；
 *   2. resize 转发 —— 本控件尺寸变化时转给 view（尺寸本身仍由宿主的布局决定）；
 *   3. idle 驱动 —— driveIdle() 手动 pump，或打开内部 QTimer(0) 自动 pump。
 *
 * 它不做任何视觉决策：不设样式表、不画背景、不设边距 —— QSS 完全穿透，
 * 宿主想怎么美化就怎么美化（"背景色"这类事由宿主决定）。
 *
 * 它也不接管卸载：七步卸载序列归 L0 session；本类只在宿主调用 detachBinding() 时
 * 解除自己与 (session, slot) 的绑定。
 *
 * 生命周期为什么是 (session, slot) 而不是裸 view 指针：
 *   本类不缓存 IPiPluginView*，每次需要时都问 session 现取。插件卸载后
 *   session 会返回 NULL，本类自动"忘掉"它 —— 从根上消灭了"控件还握着一个
 *   已释放的 view，下一次 resize 就崩"的悬垂指针问题。
 */
#ifndef PI_PLUGIN_HOST_EMBED_AREA_H
#define PI_PLUGIN_HOST_EMBED_AREA_H

#include <QWidget>

#include "pi_host_session.h"
#include "piplugin/pi_plugin.h"

class QTimer;
class QResizeEvent;

/* 宿主自己 new 出来、自己摆进布局的嵌入区域（也可以直接当容器用）。
 * 注意：销毁本控件**不等于**卸载插件；请在卸载前调用 detachBinding()。 */
class PiPluginEmbedArea : public QWidget
{
public:
    explicit PiPluginEmbedArea(QWidget* parent = nullptr);

    /* 把 session 中 slot 的插件 view 嵌进本控件。
     * set_visible 为真时顺带让 view 可见。
     * 返回 PI_E_NOINTERFACE 表示该插件没有 view（headless 插件）。 */
    PiResult attach(PiPluginHostSession* session, uint32_t slot, bool set_visible = true);

    /* 只解除本控件的绑定，不触碰 session 的卸载序列。
     * 宿主在执行 pi_plugin_host_session_unload() 之前调用它即可。 */
    void detachBinding();

    /* 当前绑定的 view（借用指针，可能为 NULL；禁止 release）。 */
    IPiPluginView* view() const;

    /* 手动 pump 一帧。何时调用（每帧 / 自己起的定时器）归宿主决定。 */
    void driveIdle();

    /* 内部 QTimer(0)：每轮事件循环 pump 一次。默认关闭 —— pump 时机归宿主；
     * 打开它适合没有自己帧时钟的宿主。 */
    void setAutoIdleEnabled(bool enabled);

protected:
    /* resize 转发：容器尺寸变了就告诉 view */
    void resizeEvent(QResizeEvent* event) override;

private:
    PiPluginHostSession* m_session;
    uint32_t             m_slot;
    QTimer*              m_idleTimer;
};

#endif /* PI_PLUGIN_HOST_EMBED_AREA_H */
