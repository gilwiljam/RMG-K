#ifndef FRAMEZERO_NETPLAY_DIALOG_HPP
#define FRAMEZERO_NETPLAY_DIALOG_HPP

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>

namespace UserInterface
{
namespace Dialog
{

/*
 * Frame Zero netplay launcher.
 *
 * Replaces the old Kaillera two-tab launcher with a single P2P-only
 * page geared at Frame Zero (rollback/GekkoNet, no server browser, no
 * Kaillera lobby). Two ways to specify the peer:
 *
 *   - Direct IP:port — used for local two-instance testing and any
 *     setup where the user can already reach the peer (LAN or hand-
 *     forwarded port). Both peers fill in the OTHER side's IP:port.
 *
 *   - Connect code (PIKA@123) — placeholder for the N02 traversal
 *     server matchmaking flow. The synchronous primitives are wired
 *     up via N02Traversal.cpp; the async session that handles
 *     HOSTOPEN/HOSTKEEP/PEER notifications still needs work, so
 *     code-based hosting is not yet hooked up here. Joining via code
 *     issues a synchronous JOIN and waits for the HOST reply.
 *
 * On Connect, the dialog stores its values in CoreSettings, sets the
 * matching FRAME_ZERO_ONLINE_* env vars, and triggers
 * MainWindow::tryFrameZeroConnect — which is the same code path the
 * env-var test harness already uses. The poll timer in MainWindow
 * drives the rest (status logging, ROM resolve, emulation start).
 */
class FrameZeroNetplayDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit FrameZeroNetplayDialog(QWidget* parent = nullptr);
    ~FrameZeroNetplayDialog() override;

    // Returns true when the user clicked Connect; the env vars are
    // pre-populated when this returns. Caller should then trigger the
    // existing tryFrameZeroConnect() path.
    bool acceptedConnect() const { return m_accepted; }

    // Snapshot the values the user picked, so the caller can configure
    // env vars even when CoreSettings is not the source of truth (e.g.
    // for a session that should not stick across restarts).
    int                       localPort()       const { return m_localPort; }
    int                       localSlot()       const { return m_localSlot; }
    int                       timeoutSeconds()  const { return m_timeoutSec; }
    QString                   peer()            const { return m_peerResolved; }

private slots:
    void onLookupCode();
    void onConnect();
    void onCancel();
    void onPeerHistoryActivated(QListWidgetItem* item);
    void onPeerTextChanged(const QString& text);

private:
    void setupUI();
    void loadSettings();
    void saveSettings();
    void appendPeerHistory(const QString& peer);
    QStringList loadPeerHistoryList() const;

    void setStatus(const QString& message, bool error);
    void refreshLocalAddressLabel();

    bool resolvePeer(QString& outResolved, QString& outError);
    bool tryParseIpPort(const QString& text, QString& outIp, int& outPort) const;

    // Synchronous JOIN flow: send N02TRAV1|JOIN|<code> to the
    // traversal server and wait for the HOST reply. Returns true and
    // fills outIpPort with "ip:port" on success.
    bool resolveCodeViaJoin(const QString& code, QString& outIpPort, QString& outError);

    QLineEdit*    m_usernameEdit    = nullptr;
    QSpinBox*     m_localPortSpin   = nullptr;
    QComboBox*    m_localSlotCombo  = nullptr;
    QSpinBox*     m_timeoutSpin     = nullptr;
    QLabel*       m_localAddrLabel  = nullptr;
    QLineEdit*    m_peerEdit        = nullptr;
    QListWidget*  m_peerHistoryList = nullptr;
    QPushButton*  m_btnLookup       = nullptr;
    QPushButton*  m_btnConnect      = nullptr;
    QPushButton*  m_btnCancel       = nullptr;
    QLabel*       m_statusLabel     = nullptr;

    bool    m_accepted    = false;
    int     m_localPort   = 7000;
    int     m_localSlot   = 0;
    int     m_timeoutSec  = 60;
    QString m_peerResolved; // "ip:port" after Connect succeeds
};

} // namespace Dialog
} // namespace UserInterface

#endif // FRAMEZERO_NETPLAY_DIALOG_HPP
