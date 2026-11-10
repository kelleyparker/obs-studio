#pragma once

#include <QObject>
#include <QPointer>

class QGraphicsDropShadowEffect;
class QPushButton;
class QVariantAnimation;
class QWidget;

/* A green heartbeat glow around the button that moves a Step-mode job on
 * (Approve & Render, Render & Continue, Use this music...). It beats only while
 * the button is visible and enabled, so clicking it (which disables or hides
 * it) stops the beat. With Windows animations off the glow holds steady. */
class YouTubeHeartbeat : public QObject {
	Q_OBJECT

public:
	explicit YouTubeHeartbeat(QPushButton *button);

	/* Lets the owner switch the beat off while the button means something else */
	void SetWanted(bool wanted);
	/* No beat while this other window is on screen (e.g. the cut editor) */
	void SetQuietWhileVisible(QWidget *other);

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	void Update();
	void SetStrength(double v);

	QPushButton *button = nullptr;
	QGraphicsDropShadowEffect *glow = nullptr;
	QVariantAnimation *beat = nullptr;
	QPointer<QWidget> quietWhile;
	bool wanted = true;
};
