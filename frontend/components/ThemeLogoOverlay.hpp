#pragma once

#include <QPixmap>
#include <QWidget>

/* The Cliffgoat Gold theme puts its logo behind the main window (see
 * Mode_CliffgoatGold.obt). The docks fill their own backgrounds, so this
 * draws the same logo, lined up with it, faintly over the whole window to
 * carry it across them. Clicks pass through. Shown only with that theme. */
class ThemeLogoOverlay : public QWidget {
	Q_OBJECT

public:
	explicit ThemeLogoOverlay(QWidget *window);

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

private:
	void Refresh();

	QPixmap logo;
};
