#include "FrameZeroNetplayDialog.hpp"

#include <RMG-Core/Settings.hpp>
#include <RMG-Core/N02Traversal.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QHostInfo>
#include <QListWidgetItem>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <stdlib.h>  // _putenv_s
#endif

namespace UserInterface
{
namespace Dialog
{

namespace
{
constexpr int kMaxHistoryEntries = 10;

void setEnvVar(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

QString trimAndStripSpaces(const QString& in)
{
    QString s = in.trimmed();
    s.remove(' ');
    return s;
}
} // namespace

FrameZeroNetplayDialog::FrameZeroNetplayDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Frame Zero Netplay");
    setModal(true);
    setupUI();
    loadSettings();
}

FrameZeroNetplayDialog::~FrameZeroNetplayDialog() = default;

void FrameZeroNetplayDialog::setupUI()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    // ---- Local identity / port ----
    auto* localBox    = new QGroupBox("Your side", this);
    auto* localForm   = new QFormLayout(localBox);

    m_usernameEdit = new QLineEdit(localBox);
    m_usernameEdit->setMaxLength(16);
    m_usernameEdit->setPlaceholderText("Player");

    m_localPortSpin = new QSpinBox(localBox);
    m_localPortSpin->setRange(1024, 65535);
    m_localPortSpin->setValue(7000);

    m_localSlotCombo = new QComboBox(localBox);
    m_localSlotCombo->addItem("Player 1", 0);
    m_localSlotCombo->addItem("Player 2", 1);

    m_inputDelaySpin = new QSpinBox(localBox);
    m_inputDelaySpin->setRange(0, 9);
    m_inputDelaySpin->setSuffix(" frames");
    m_inputDelaySpin->setValue(2);
    m_inputDelaySpin->setToolTip("Frames of artificial input delay added by GekkoNet. "
                                 "Higher delay = less rollback work but more felt input lag. "
                                 "2 is the typical default.");

    m_timeoutSpin = new QSpinBox(localBox);
    m_timeoutSpin->setRange(5, 600);
    m_timeoutSpin->setSuffix(" s");
    m_timeoutSpin->setValue(60);

    m_localAddrLabel = new QLabel(localBox);
    m_localAddrLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_localAddrLabel->setWordWrap(true);
    m_localAddrLabel->setToolTip("LAN addresses your peer can use to reach you. "
                                 "For internet play you'll need to share your public IP "
                                 "(or use a connect code).");

    localForm->addRow("Username:",       m_usernameEdit);
    localForm->addRow("Local UDP port:", m_localPortSpin);
    localForm->addRow("Play as:",        m_localSlotCombo);
    localForm->addRow("Input delay:",    m_inputDelaySpin);
    localForm->addRow("Handshake timeout:", m_timeoutSpin);
    localForm->addRow("Share with peer:", m_localAddrLabel);

    root->addWidget(localBox);

    // ---- Peer ----
    auto* peerBox  = new QGroupBox("Peer", this);
    auto* peerLay  = new QVBoxLayout(peerBox);

    auto* peerHint = new QLabel(
        "Enter the peer's <b>IP:port</b> (e.g. <code>192.168.1.5:7001</code>) "
        "for direct connections, or a <b>connect code</b> "
        "(e.g. <code>PIKA@1</code>) to look the peer up via the N02 traversal "
        "server.",
        peerBox);
    peerHint->setWordWrap(true);

    auto* peerInputRow = new QHBoxLayout();
    m_peerEdit = new QLineEdit(peerBox);
    m_peerEdit->setPlaceholderText("ip:port  or  CODE@123");
    m_btnLookup = new QPushButton("Look up code", peerBox);
    m_btnLookup->setToolTip("Resolve a connect code to an IP:port via the traversal server "
                            "(synchronous JOIN — the host must be online).");
    m_btnLookup->setEnabled(false);
    peerInputRow->addWidget(m_peerEdit, 1);
    peerInputRow->addWidget(m_btnLookup);

    m_peerHistoryList = new QListWidget(peerBox);
    m_peerHistoryList->setToolTip("Double-click to use a previous peer.");
    m_peerHistoryList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_peerHistoryList->setMaximumHeight(96);

    peerLay->addWidget(peerHint);
    peerLay->addLayout(peerInputRow);
    peerLay->addWidget(new QLabel("Recent peers:", peerBox));
    peerLay->addWidget(m_peerHistoryList);

    root->addWidget(peerBox);

    // ---- Status ----
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setMinimumHeight(QFontMetrics(m_statusLabel->font()).lineSpacing() * 2);
    m_statusLabel->setTextFormat(Qt::RichText);
    root->addWidget(m_statusLabel);

    // ---- Buttons ----
    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    m_btnCancel  = new QPushButton("Cancel",  this);
    m_btnConnect = new QPushButton("Connect", this);
    m_btnConnect->setDefault(true);
    buttonRow->addWidget(m_btnCancel);
    buttonRow->addWidget(m_btnConnect);
    root->addLayout(buttonRow);

    connect(m_btnConnect,    &QPushButton::clicked, this, &FrameZeroNetplayDialog::onConnect);
    connect(m_btnCancel,     &QPushButton::clicked, this, &FrameZeroNetplayDialog::onCancel);
    connect(m_btnLookup,     &QPushButton::clicked, this, &FrameZeroNetplayDialog::onLookupCode);
    connect(m_peerEdit,      &QLineEdit::textChanged, this, &FrameZeroNetplayDialog::onPeerTextChanged);
    connect(m_peerHistoryList, &QListWidget::itemActivated, this, &FrameZeroNetplayDialog::onPeerHistoryActivated);
    connect(m_peerHistoryList, &QListWidget::itemDoubleClicked, this, &FrameZeroNetplayDialog::onPeerHistoryActivated);
    connect(m_localPortSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { refreshLocalAddressLabel(); });

    refreshLocalAddressLabel();
    resize(480, 460);
}

void FrameZeroNetplayDialog::loadSettings()
{
    const std::string username  = CoreSettingsGetStringValue(SettingsID::FrameZero_Username);
    const int         localPort = CoreSettingsGetIntValue(SettingsID::FrameZero_LocalPort);
    const int         localSlot = CoreSettingsGetIntValue(SettingsID::FrameZero_LocalSlot);
    const int         inputDelay = CoreSettingsGetIntValue(SettingsID::FrameZero_InputDelay);
    const int         timeoutS  = CoreSettingsGetIntValue(SettingsID::FrameZero_TimeoutSeconds);
    const std::string lastPeer  = CoreSettingsGetStringValue(SettingsID::FrameZero_LastPeer);

    m_usernameEdit->setText(QString::fromStdString(username));
    if (localPort >= 1024 && localPort <= 65535) m_localPortSpin->setValue(localPort);
    if (localSlot == 0 || localSlot == 1) m_localSlotCombo->setCurrentIndex(localSlot);
    if (inputDelay >= 0 && inputDelay <= 9) m_inputDelaySpin->setValue(inputDelay);
    if (timeoutS  >= 5 && timeoutS  <= 600) m_timeoutSpin->setValue(timeoutS);
    m_peerEdit->setText(QString::fromStdString(lastPeer));

    m_peerHistoryList->clear();
    for (const QString& entry : loadPeerHistoryList())
    {
        m_peerHistoryList->addItem(entry);
    }

    const std::string geom = CoreSettingsGetStringValue(SettingsID::FrameZero_DialogGeometry);
    if (!geom.empty())
    {
        restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geom)));
    }

    onPeerTextChanged(m_peerEdit->text());
}

void FrameZeroNetplayDialog::saveSettings()
{
    CoreSettingsSetValue(SettingsID::FrameZero_Username,
                         m_usernameEdit->text().toStdString());
    CoreSettingsSetValue(SettingsID::FrameZero_LocalPort,
                         m_localPortSpin->value());
    CoreSettingsSetValue(SettingsID::FrameZero_LocalSlot,
                         m_localSlotCombo->currentIndex());
    CoreSettingsSetValue(SettingsID::FrameZero_InputDelay,
                         m_inputDelaySpin->value());
    CoreSettingsSetValue(SettingsID::FrameZero_TimeoutSeconds,
                         m_timeoutSpin->value());
    CoreSettingsSetValue(SettingsID::FrameZero_LastPeer,
                         m_peerResolved.toStdString());
    CoreSettingsSetValue(SettingsID::FrameZero_DialogGeometry,
                         saveGeometry().toBase64().toStdString());
    CoreSettingsSave();
}

QStringList FrameZeroNetplayDialog::loadPeerHistoryList() const
{
    const std::string raw = CoreSettingsGetStringValue(SettingsID::FrameZero_PeerHistory);
    if (raw.empty()) return {};
    QStringList parts = QString::fromStdString(raw).split('\n', Qt::SkipEmptyParts);
    parts.removeDuplicates();
    if (parts.size() > kMaxHistoryEntries)
    {
        parts = parts.mid(0, kMaxHistoryEntries);
    }
    return parts;
}

void FrameZeroNetplayDialog::appendPeerHistory(const QString& peer)
{
    if (peer.isEmpty()) return;
    QStringList history = loadPeerHistoryList();
    history.removeAll(peer);
    history.prepend(peer);
    if (history.size() > kMaxHistoryEntries)
    {
        history = history.mid(0, kMaxHistoryEntries);
    }
    CoreSettingsSetValue(SettingsID::FrameZero_PeerHistory,
                         history.join('\n').toStdString());
}

void FrameZeroNetplayDialog::refreshLocalAddressLabel()
{
    if (m_localAddrLabel == nullptr) return;

    QStringList addrs;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
    {
        if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
        if (a.isLoopback()) continue;
        if (a.isLinkLocal()) continue;
        addrs << a.toString();
    }

    const int port = m_localPortSpin ? m_localPortSpin->value() : 7000;
    if (addrs.isEmpty())
    {
        m_localAddrLabel->setText("<i>(no LAN address detected — only loopback "
                                  "<code>127.0.0.1:" + QString::number(port) +
                                  "</code> is available)</i>");
        return;
    }

    QStringList lines;
    lines.reserve(addrs.size());
    for (const QString& ip : addrs)
    {
        lines << "<code>" + ip + ":" + QString::number(port) + "</code>";
    }
    m_localAddrLabel->setText(lines.join("&nbsp;&nbsp;"));
}

void FrameZeroNetplayDialog::setStatus(const QString& message, bool error)
{
    if (message.isEmpty())
    {
        m_statusLabel->clear();
        m_statusLabel->setStyleSheet(QString());
        return;
    }
    m_statusLabel->setStyleSheet(error ? "color:#b00020;" : "color:#0b6e2e;");
    m_statusLabel->setText(message);
}

bool FrameZeroNetplayDialog::tryParseIpPort(const QString& text, QString& outIp, int& outPort) const
{
    const QString trimmed = trimAndStripSpaces(text);
    if (trimmed.isEmpty()) return false;

    const int colon = trimmed.lastIndexOf(':');
    if (colon <= 0 || colon == trimmed.size() - 1) return false;

    QString hostPart = trimmed.left(colon);
    QString portPart = trimmed.mid(colon + 1);

    bool portOk = false;
    int  port   = portPart.toInt(&portOk);
    if (!portOk || port <= 0 || port > 65535) return false;

    QHostAddress addr;
    if (addr.setAddress(hostPart))
    {
        outIp   = addr.toString();
        outPort = port;
        return true;
    }

    // Hostname — try DNS so the user can type "myhost:7000".
    const QHostInfo hostInfo = QHostInfo::fromName(hostPart);
    for (const QHostAddress& candidate : hostInfo.addresses())
    {
        if (candidate.protocol() == QAbstractSocket::IPv4Protocol)
        {
            outIp   = candidate.toString();
            outPort = port;
            return true;
        }
    }
    return false;
}

bool FrameZeroNetplayDialog::resolveCodeViaJoin(const QString& code, QString& outIpPort, QString& outError)
{
    const std::string normalized = CoreN02NormalizeCode(code.toStdString());
    if (normalized.empty())
    {
        outError = "That doesn't look like a connect code.";
        return false;
    }

    // We need a single UDP socket for both the JOIN and the HOST
    // reply, so we can't reuse CoreN02SendRequest (which closes the
    // socket after one reply). Drive it with QUdpSocket here.
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DefaultForPlatform))
    {
        outError = "Could not open a UDP socket for the lookup.";
        return false;
    }

    QHostAddress server;
    bool resolved = false;
    if (server.setAddress(QString::fromUtf8(CoreN02Traversal::kHost)))
    {
        resolved = true;
    }
    else
    {
        const QHostInfo info = QHostInfo::fromName(QString::fromUtf8(CoreN02Traversal::kHost));
        for (const QHostAddress& a : info.addresses())
        {
            if (a.protocol() == QAbstractSocket::IPv4Protocol)
            {
                server = a;
                resolved = true;
                break;
            }
        }
    }
    if (!resolved)
    {
        outError = "Could not resolve the traversal server address.";
        return false;
    }

    // Send up to 3 JOINs at 700 ms intervals; each JOIN includes a
    // monotonic tag so the server can dedupe retries.
    const QByteArray protocol = CoreN02Traversal::kProtocol;
    const QByteArray codeBytes = QString::fromStdString(normalized).toUtf8();

    auto sendJoin = [&](quint32 tag) {
        QByteArray msg = protocol + "|JOIN|" + codeBytes + "|" +
                         QByteArray::number(tag);
        socket.writeDatagram(msg, server, CoreN02Traversal::kPort);
    };

    QElapsedTimer overall;
    overall.start();
    quint32 tag = 1;
    sendJoin(tag++);

    while (overall.elapsed() < 5000)
    {
        // Wait for a reply, but no longer than the next retry slot.
        const int wait = std::min(700, (int)(5000 - overall.elapsed()));
        if (wait <= 0) break;
        if (!socket.waitForReadyRead(wait))
        {
            sendJoin(tag++);
            continue;
        }

        while (socket.hasPendingDatagrams())
        {
            QByteArray buf;
            buf.resize((int)socket.pendingDatagramSize());
            socket.readDatagram(buf.data(), buf.size());

            // Strip leading NUL byte (matches existing traversal server quirks).
            if (!buf.isEmpty() && buf[0] == '\0') buf.remove(0, 1);

            QList<QByteArray> parts = buf.split('|');
            if (parts.size() < 2 || parts[0] != protocol) continue;

            const QByteArray& type = parts[1];
            if (type == "HOST" && parts.size() >= 5)
            {
                const QString hostIp = QString::fromUtf8(parts[3]);
                const int     port   = parts[4].toInt();
                if (port <= 0)
                {
                    outError = "Traversal server returned an invalid host port.";
                    return false;
                }
                outIpPort = hostIp + ":" + QString::number(port);
                return true;
            }
            if (type == "ERR" && parts.size() >= 3)
            {
                outError = "Traversal server: " + QString::fromUtf8(parts[2]);
                return false;
            }
            // PUNCH/OK — keep listening.
        }
    }

    outError = "Traversal server didn't respond with a host endpoint within 5s. "
               "Make sure the host is online and you typed the code right.";
    return false;
}

bool FrameZeroNetplayDialog::resolvePeer(QString& outResolved, QString& outError)
{
    const QString peerText = trimAndStripSpaces(m_peerEdit->text());
    if (peerText.isEmpty())
    {
        outError = "Enter a peer IP:port or a connect code.";
        return false;
    }

    QString ip;
    int port = 0;
    if (tryParseIpPort(peerText, ip, port))
    {
        outResolved = ip + ":" + QString::number(port);
        return true;
    }

    if (CoreN02LooksLikeCode(peerText.toStdString()))
    {
        setStatus("Asking the traversal server about " + peerText.toUpper() + "...", false);
        QApplication::processEvents();
        return resolveCodeViaJoin(peerText, outResolved, outError);
    }

    outError = "Couldn't parse \"" + peerText + "\" as either an IP:port or a connect code.";
    return false;
}

void FrameZeroNetplayDialog::onPeerTextChanged(const QString& text)
{
    const QString trimmed = trimAndStripSpaces(text);
    const bool isCode = CoreN02LooksLikeCode(trimmed.toStdString());
    m_btnLookup->setEnabled(isCode);
}

void FrameZeroNetplayDialog::onPeerHistoryActivated(QListWidgetItem* item)
{
    if (item == nullptr) return;
    m_peerEdit->setText(item->text());
    m_peerEdit->setFocus();
}

void FrameZeroNetplayDialog::onLookupCode()
{
    const QString peerText = trimAndStripSpaces(m_peerEdit->text());
    if (!CoreN02LooksLikeCode(peerText.toStdString())) return;

    setStatus("Looking up " + peerText.toUpper() + "...", false);
    m_btnLookup->setEnabled(false);
    m_btnConnect->setEnabled(false);
    QApplication::processEvents();

    QString resolved;
    QString error;
    const bool ok = resolveCodeViaJoin(peerText, resolved, error);

    m_btnLookup->setEnabled(true);
    m_btnConnect->setEnabled(true);

    if (ok)
    {
        setStatus("Resolved <b>" + peerText.toUpper() + "</b> → <code>" + resolved +
                  "</code>. Press Connect to start the handshake.", false);
        m_peerResolved = resolved;
    }
    else
    {
        setStatus(error, true);
        m_peerResolved.clear();
    }
}

void FrameZeroNetplayDialog::onConnect()
{
    setStatus(QString(), false);

    QString resolved;
    QString error;
    if (!resolvePeer(resolved, error))
    {
        setStatus(error, true);
        return;
    }

    // Snapshot the values for the caller.
    m_localPort    = m_localPortSpin->value();
    m_localSlot    = m_localSlotCombo->currentIndex();
    m_timeoutSec   = m_timeoutSpin->value();
    m_peerResolved = resolved;

    // Push to env vars so the existing tryFrameZeroConnect() +
    // OnlineArm() in Emulation.cpp pick them up. SETTLE is left
    // unset here — the poll timer flips it to 1 once the handshake
    // succeeds, which is the same behaviour as the env-var harness.
    setEnvVar("FRAME_ZERO_ONLINE",         "1");
    setEnvVar("FRAME_ZERO_ONLINE_PORT",    std::to_string(m_localPort));
    setEnvVar("FRAME_ZERO_ONLINE_PEERS",   resolved.toStdString());
    setEnvVar("FRAME_ZERO_ONLINE_LOCAL",   std::to_string(m_localSlot));
    setEnvVar("FRAME_ZERO_ONLINE_DELAY",   std::to_string(m_inputDelaySpin->value()));
    setEnvVar("FRAME_ZERO_ONLINE_TIMEOUT", std::to_string(m_timeoutSec));

    appendPeerHistory(resolved);
    saveSettings();

    m_accepted = true;
    accept();
}

void FrameZeroNetplayDialog::onCancel()
{
    saveSettings();
    reject();
}

} // namespace Dialog
} // namespace UserInterface
