#include "YouTubeCutEditor.hpp"

#include <OBSApp.hpp>

#include <QAudioOutput>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDial>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QTableWidget>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kMinCutSec = 0.10;   /* a cut shorter than this does nothing (same as the pipeline) */
constexpr double kDialStepSec = 0.05; /* one dial notch */
constexpr int kEdgeDialSteps = 60;    /* ±3.0 s per edge */
constexpr int kGlobalDialSteps = 20;  /* ±1.0 s for all cuts */
constexpr int kEdgeGrabPx = 6;

enum Column { ColCut, ColKind, ColStart, ColEnd, ColLength, ColReason };

QString FormatTime(double seconds)
{
	seconds = std::max(0.0, seconds);
	int minutes = int(seconds / 60.0);
	return QString("%1:%2").arg(minutes).arg(seconds - minutes * 60.0, 4, 'f', 1, QChar('0'));
}

QString FormatSigned(double seconds)
{
	return QString("%1%2s").arg(seconds >= 0.0 ? "+" : "").arg(seconds, 0, 'f', 2);
}

} // namespace

/* ------------------------------------------------------------------------ */
/* CutTimelineWidget                                                        */

CutTimelineWidget::CutTimelineWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(64);
	setMouseTracking(true);
	setToolTip(QString());
}

void CutTimelineWidget::SetData(QVector<YouTubeCut> *cuts_, double duration_)
{
	cuts = cuts_;
	duration = duration_;
	viewStart = 0.0;
	viewEnd = duration_;
	update();
}

void CutTimelineWidget::SetGlobalOffset(double seconds)
{
	globalOffset = seconds;
	update();
}

void CutTimelineWidget::SetPlayhead(double seconds)
{
	playhead = seconds;
	/* Keep the playhead in view while zoomed in */
	double span = viewEnd - viewStart;
	if (span < duration && (seconds < viewStart || seconds > viewEnd)) {
		viewStart = std::clamp(seconds - span * 0.1, 0.0, std::max(0.0, duration - span));
		viewEnd = viewStart + span;
	}
	update();
}

void CutTimelineWidget::SetSelected(int index)
{
	selected = index;
	update();
}

double CutTimelineWidget::EffectiveStart(const YouTubeCut &cut) const
{
	return std::clamp(cut.start - globalOffset, 0.0, duration);
}

double CutTimelineWidget::EffectiveEnd(const YouTubeCut &cut) const
{
	return std::clamp(cut.end + globalOffset, 0.0, duration);
}

double CutTimelineWidget::XToTime(double x) const
{
	if (width() <= 0) {
		return viewStart;
	}
	return std::clamp(viewStart + (viewEnd - viewStart) * x / width(), 0.0, duration);
}

double CutTimelineWidget::TimeToX(double t) const
{
	if (viewEnd <= viewStart) {
		return 0.0;
	}
	return (t - viewStart) / (viewEnd - viewStart) * width();
}

int CutTimelineWidget::EdgeAt(const QPoint &pos, bool *startEdge) const
{
	if (!cuts) {
		return -1;
	}
	for (int i = 0; i < cuts->size(); i++) {
		const YouTubeCut &c = (*cuts)[i];
		if (std::abs(TimeToX(EffectiveStart(c)) - pos.x()) <= kEdgeGrabPx) {
			*startEdge = true;
			return i;
		}
		if (std::abs(TimeToX(EffectiveEnd(c)) - pos.x()) <= kEdgeGrabPx) {
			*startEdge = false;
			return i;
		}
	}
	return -1;
}

int CutTimelineWidget::CutAt(double t) const
{
	if (!cuts) {
		return -1;
	}
	for (int i = 0; i < cuts->size(); i++) {
		const YouTubeCut &c = (*cuts)[i];
		if (t >= EffectiveStart(c) && t <= EffectiveEnd(c)) {
			return i;
		}
	}
	return -1;
}

void CutTimelineWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);
	QRect bar = rect().adjusted(0, 14, 0, -4);
	p.fillRect(rect(), QColor(20, 14, 28));
	p.fillRect(bar, QColor(58, 120, 72)); /* kept footage */

	/* Second ticks along the top */
	double span = viewEnd - viewStart;
	double tick = span > 600 ? 60 : span > 120 ? 10 : span > 30 ? 5 : 1;
	p.setPen(QColor(150, 140, 170));
	QFont f = font();
	f.setPointSizeF(7.5);
	p.setFont(f);
	for (double t = std::ceil(viewStart / tick) * tick; t <= viewEnd; t += tick) {
		int x = int(TimeToX(t));
		p.drawLine(x, 10, x, 14);
		if (int(t / tick) % (span > 600 ? 5 : 5) == 0) {
			p.drawText(x + 2, 9, FormatTime(t));
		}
	}

	if (cuts) {
		for (int i = 0; i < cuts->size(); i++) {
			const YouTubeCut &c = (*cuts)[i];
			double s = EffectiveStart(c);
			double e = EffectiveEnd(c);
			QRect r(QPoint(int(TimeToX(s)), bar.top()), QPoint(std::max(int(TimeToX(e)), int(TimeToX(s)) + 1), bar.bottom()));
			bool active = c.enabled && e - s >= kMinCutSec;
			if (active) {
				p.fillRect(r, c.kind == "retake" ? QColor(235, 140, 40) : QColor(205, 50, 60));
			} else {
				p.fillRect(r, QBrush(QColor(120, 120, 130), Qt::BDiagPattern));
			}
			if (i == selected) {
				p.setPen(QPen(QColor(255, 255, 255), 2));
				p.drawRect(r.adjusted(0, 0, -1, -1));
			}
		}
	}

	/* Playhead */
	int px = int(TimeToX(playhead));
	p.setPen(QPen(QColor(255, 240, 80), 2));
	p.drawLine(px, 0, px, height());
}

void CutTimelineWidget::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || duration <= 0.0) {
		return;
	}
	bool startEdge = false;
	int edge = EdgeAt(event->pos(), &startEdge);
	if (edge >= 0) {
		dragCut = edge;
		dragStartEdge = startEdge;
		emit CutSelected(edge);
		return;
	}
	double t = XToTime(event->position().x());
	int cut = CutAt(t);
	if (cut >= 0) {
		emit CutSelected(cut);
	}
	emit SeekRequested(t);
}

void CutTimelineWidget::mouseMoveEvent(QMouseEvent *event)
{
	if (dragCut >= 0 && cuts) {
		/* Store the per-cut edge, so the "All cuts" offset still applies on top */
		double t = XToTime(event->position().x());
		YouTubeCut &c = (*cuts)[dragCut];
		if (dragStartEdge) {
			c.start = t + globalOffset;
		} else {
			c.end = t - globalOffset;
		}
		emit CutEdited(dragCut);
		update();
		return;
	}

	bool startEdge = false;
	setCursor(EdgeAt(event->pos(), &startEdge) >= 0 ? Qt::SizeHorCursor : Qt::PointingHandCursor);
	int cut = CutAt(XToTime(event->position().x()));
	if (cut >= 0 && cuts) {
		const YouTubeCut &c = (*cuts)[cut];
		QToolTip::showText(event->globalPosition().toPoint(),
				   QString("%1 cut, %2s\n%3")
					   .arg(c.kind == "retake" ? "Retake" : "Dead air")
					   .arg(EffectiveEnd(c) - EffectiveStart(c), 0, 'f', 2)
					   .arg(c.reason),
				   this);
	} else {
		QToolTip::hideText();
	}
}

void CutTimelineWidget::mouseReleaseEvent(QMouseEvent *)
{
	dragCut = -1;
}

void CutTimelineWidget::wheelEvent(QWheelEvent *event)
{
	if (duration <= 0.0) {
		return;
	}
	double anchor = XToTime(event->position().x());
	double span = viewEnd - viewStart;
	double factor = event->angleDelta().y() > 0 ? 0.7 : 1.0 / 0.7;
	double newSpan = std::clamp(span * factor, std::min(5.0, duration), duration);
	double ratio = (anchor - viewStart) / span;
	viewStart = std::clamp(anchor - newSpan * ratio, 0.0, duration - newSpan);
	viewEnd = viewStart + newSpan;
	update();
	event->accept();
}

/* ------------------------------------------------------------------------ */
/* YouTubeCutEditor                                                         */

YouTubeCutEditor::YouTubeCutEditor(QWidget *parent) : QDialog(parent)
{
	setWindowTitle("YouTube Cut Editor — review dead air & retakes");
	setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	resize(1200, 820);

	player = new QMediaPlayer(this);
	audio = new QAudioOutput(this);
	player->setAudioOutput(audio);
	video = new QVideoWidget(this);
	video->setMinimumSize(480, 270);
	player->setVideoOutput(video);

	timeline = new CutTimelineWidget(this);

	/* Transport */
	playButton = new QPushButton("▶ Play", this);
	QPushButton *backButton = new QPushButton("⏪ 5s", this);
	QPushButton *forwardButton = new QPushButton("5s ⏩", this);
	QPushButton *prevCutButton = new QPushButton("⏮ Prev cut", this);
	QPushButton *nextCutButton = new QPushButton("Next cut ⏭", this);
	previewCheck = new QCheckBox("Preview edit (skip cuts while playing)", this);
	previewCheck->setChecked(true);
	timeLabel = new QLabel(this);
	lengthLabel = new QLabel(this);
	lengthLabel->setStyleSheet("font-weight: bold; color: #bfe3c0;");

	QHBoxLayout *transport = new QHBoxLayout();
	for (QWidget *w : {(QWidget *)prevCutButton, (QWidget *)backButton, (QWidget *)playButton,
			   (QWidget *)forwardButton, (QWidget *)nextCutButton}) {
		transport->addWidget(w);
	}
	transport->addSpacing(12);
	transport->addWidget(previewCheck);
	transport->addStretch();
	transport->addWidget(timeLabel);
	transport->addSpacing(12);
	transport->addWidget(lengthLabel);

	/* Cut list */
	table = new QTableWidget(0, 6, this);
	table->setHorizontalHeaderLabels({"Cut", "Kind", "Start", "End", "Length", "Reason"});
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->verticalHeader()->hide();
	table->horizontalHeader()->setSectionResizeMode(ColReason, QHeaderView::Stretch);
	for (int col : {ColCut, ColKind, ColStart, ColEnd, ColLength}) {
		table->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
	}

	/* Dials */
	auto makeDial = [this](int steps) {
		QDial *dial = new QDial(this);
		dial->setRange(-steps, steps);
		dial->setNotchesVisible(true);
		dial->setWrapping(false);
		dial->setFixedSize(84, 84);
		return dial;
	};
	startDial = makeDial(kEdgeDialSteps);
	endDial = makeDial(kEdgeDialSteps);
	globalDial = makeDial(kGlobalDialSteps);
	startDialLabel = new QLabel(this);
	endDialLabel = new QLabel(this);
	globalDialLabel = new QLabel(this);
	for (QLabel *l : {startDialLabel, endDialLabel, globalDialLabel}) {
		l->setAlignment(Qt::AlignCenter);
	}
	cutInfoLabel = new QLabel("Select a cut in the list or on the timeline.", this);
	cutInfoLabel->setWordWrap(true);

	QPushButton *undoCutButton = new QPushButton("Undo this cut", this);
	QPushButton *resetCutButton = new QPushButton("Reset", this);

	QGroupBox *selectedGroup = new QGroupBox("Selected cut", this);
	QGridLayout *selGrid = new QGridLayout(selectedGroup);
	selGrid->addWidget(cutInfoLabel, 0, 0, 1, 2);
	selGrid->addWidget(new QLabel("Cut start", this), 1, 0, Qt::AlignCenter);
	selGrid->addWidget(new QLabel("Cut end", this), 1, 1, Qt::AlignCenter);
	selGrid->addWidget(startDial, 2, 0, Qt::AlignCenter);
	selGrid->addWidget(endDial, 2, 1, Qt::AlignCenter);
	selGrid->addWidget(startDialLabel, 3, 0);
	selGrid->addWidget(endDialLabel, 3, 1);
	selGrid->addWidget(undoCutButton, 4, 0);
	selGrid->addWidget(resetCutButton, 4, 1);

	QGroupBox *globalGroup = new QGroupBox("All cuts", this);
	QVBoxLayout *globalLayout = new QVBoxLayout(globalGroup);
	QLabel *globalHint = new QLabel("Turn left to keep more around every cut,\nright to cut more.", this);
	globalHint->setAlignment(Qt::AlignCenter);
	globalLayout->addWidget(globalHint);
	globalLayout->addWidget(globalDial, 0, Qt::AlignCenter);
	globalLayout->addWidget(globalDialLabel);

	QVBoxLayout *sideLayout = new QVBoxLayout();
	sideLayout->addWidget(selectedGroup);
	sideLayout->addWidget(globalGroup);
	sideLayout->addStretch();

	QHBoxLayout *lower = new QHBoxLayout();
	lower->addWidget(table, 1);
	lower->addLayout(sideLayout);

	QWidget *lowerWidget = new QWidget(this);
	QVBoxLayout *lowerOuter = new QVBoxLayout(lowerWidget);
	lowerOuter->setContentsMargins(0, 0, 0, 0);
	lowerOuter->addLayout(lower);

	QSplitter *splitter = new QSplitter(Qt::Vertical, this);
	splitter->addWidget(video);
	splitter->addWidget(lowerWidget);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);

	QPushButton *approveButton = new QPushButton("✅ Approve && Render", this);
	approveButton->setStyleSheet("font-weight: bold;");
	QPushButton *closeButton = new QPushButton("Close (keep waiting)", this);
	QLabel *legend = new QLabel(
		"<span style='color:#cd323c'>■</span> dead air &nbsp; <span style='color:#eb8c28'>■</span> retake &nbsp; "
		"<span style='color:#3a7848'>■</span> kept &nbsp; Drag a cut's edge to move it, scroll to zoom. "
		"Space play/pause, ←/→ 5s, [ / ] previous/next cut.",
		this);
	QHBoxLayout *bottom = new QHBoxLayout();
	bottom->addWidget(legend, 1);
	bottom->addWidget(closeButton);
	bottom->addWidget(approveButton);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(splitter, 1);
	layout->addWidget(timeline);
	layout->addLayout(transport);
	layout->addLayout(bottom);

	/* Remember the window size */
	const char *geometry = config_get_string(App()->GetUserConfig(), "YouTubeWorkflow", "CutEditorGeometry");
	if (geometry && *geometry) {
		restoreGeometry(QByteArray::fromBase64(geometry));
	}

	/* Wiring */
	connect(playButton, &QPushButton::clicked, this, [this]() {
		if (player->playbackState() == QMediaPlayer::PlayingState) {
			player->pause();
		} else {
			player->play();
		}
	});
	connect(player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
		bool playing = state == QMediaPlayer::PlayingState;
		playButton->setText(playing ? "⏸ Pause" : "▶ Play");
		if (playing) {
			previewTimer.start();
		} else {
			previewTimer.stop();
		}
	});
	connect(player, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
		double t = ms / 1000.0;
		timeline->SetPlayhead(t);
		timeLabel->setText(QString("%1 / %2").arg(FormatTime(t), FormatTime(duration)));
	});
	connect(player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
		timeLabel->setText(QString("Video error: %1").arg(message));
	});
	previewTimer.setInterval(25);
	connect(&previewTimer, &QTimer::timeout, this, &YouTubeCutEditor::SkipCutsWhilePlaying);

	connect(backButton, &QPushButton::clicked, this, [this]() { Seek(Position() - 5.0); });
	connect(forwardButton, &QPushButton::clicked, this, [this]() { Seek(Position() + 5.0); });
	connect(prevCutButton, &QPushButton::clicked, this, [this]() { StepCut(-1); });
	connect(nextCutButton, &QPushButton::clicked, this, [this]() { StepCut(1); });

	connect(timeline, &CutTimelineWidget::SeekRequested, this, &YouTubeCutEditor::Seek);
	connect(timeline, &CutTimelineWidget::CutSelected, this, [this](int index) { SelectCut(index, false); });
	connect(timeline, &CutTimelineWidget::CutEdited, this, [this](int index) {
		ClampCut(index);
		RefreshRow(index);
		RefreshSelectedInfo();
		RefreshLength();
	});

	connect(table, &QTableWidget::cellClicked, this, [this](int row, int column) {
		if (column == ColCut) {
			cuts[row].enabled = !cuts[row].enabled;
			RefreshRow(row);
			RefreshLength();
			timeline->update();
		}
		SelectCut(row, true);
	});

	connect(startDial, &QDial::valueChanged, this, &YouTubeCutEditor::OnDialsChanged);
	connect(endDial, &QDial::valueChanged, this, &YouTubeCutEditor::OnDialsChanged);
	connect(globalDial, &QDial::valueChanged, this, &YouTubeCutEditor::OnGlobalDialChanged);
	connect(undoCutButton, &QPushButton::clicked, this, &YouTubeCutEditor::ToggleSelectedCut);
	connect(resetCutButton, &QPushButton::clicked, this, &YouTubeCutEditor::ResetSelectedCut);

	connect(approveButton, &QPushButton::clicked, this, [this]() {
		if (Save()) {
			player->stop();
			emit Approved(statePath);
			hide();
		}
	});
	connect(closeButton, &QPushButton::clicked, this, &QWidget::close);

	auto shortcut = [this](const QKeySequence &key, auto slot) {
		QShortcut *s = new QShortcut(key, this);
		connect(s, &QShortcut::activated, this, slot);
	};
	shortcut(Qt::Key_Space, [playButton = playButton]() { playButton->click(); });
	shortcut(Qt::Key_Left, [this]() { Seek(Position() - 5.0); });
	shortcut(Qt::Key_Right, [this]() { Seek(Position() + 5.0); });
	shortcut(Qt::Key_BracketLeft, [this]() { StepCut(-1); });
	shortcut(Qt::Key_BracketRight, [this]() { StepCut(1); });

	OnGlobalDialChanged();
	RefreshSelectedInfo();
}

YouTubeCutEditor::~YouTubeCutEditor()
{
	config_set_string(App()->GetUserConfig(), "YouTubeWorkflow", "CutEditorGeometry",
			  saveGeometry().toBase64().constData());
}

void YouTubeCutEditor::closeEvent(QCloseEvent *event)
{
	player->pause();
	config_set_string(App()->GetUserConfig(), "YouTubeWorkflow", "CutEditorGeometry",
			  saveGeometry().toBase64().constData());
	QDialog::closeEvent(event);
}

bool YouTubeCutEditor::Load(const QString &cutsPath_, const QString &statePath_)
{
	QFile file(cutsPath_);
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();

	cutsPath = cutsPath_;
	statePath = statePath_;
	duration = root.value("duration").toDouble();
	cuts.clear();
	for (const QJsonValue &v : root.value("cuts").toArray()) {
		QJsonObject o = v.toObject();
		YouTubeCut cut;
		cut.id = o.value("id").toString();
		cut.kind = o.value("kind").toString("silence");
		cut.reason = o.value("reason").toString();
		cut.origStart = o.contains("orig_start") ? o.value("orig_start").toDouble() : o.value("start").toDouble();
		cut.origEnd = o.contains("orig_end") ? o.value("orig_end").toDouble() : o.value("end").toDouble();
		cut.start = o.value("start").toDouble();
		cut.end = o.value("end").toDouble();
		cut.enabled = o.value("enabled").toBool(true);
		cuts.append(cut);
	}

	updating = true;
	globalDial->setValue(0);
	updating = false;
	timeline->SetGlobalOffset(0.0);
	timeline->SetData(&cuts, duration);
	selected = -1;
	timeline->SetSelected(-1);
	RefreshTable();
	RefreshSelectedInfo();
	RefreshLength();
	OnGlobalDialChanged();

	player->setSource(QUrl::fromLocalFile(root.value("video").toString()));
	timeLabel->setText(QString("0:00.0 / %1").arg(FormatTime(duration)));
	setWindowTitle(QString("YouTube Cut Editor — %1").arg(QFileInfo(root.value("video").toString()).fileName()));
	return true;
}

double YouTubeCutEditor::Position() const
{
	return player->position() / 1000.0;
}

void YouTubeCutEditor::Seek(double seconds)
{
	player->setPosition(qint64(std::clamp(seconds, 0.0, duration) * 1000.0));
}

void YouTubeCutEditor::SkipCutsWhilePlaying()
{
	if (!previewCheck->isChecked()) {
		return;
	}
	double t = Position();
	for (const YouTubeCut &c : cuts) {
		double s = timeline->EffectiveStart(c);
		double e = timeline->EffectiveEnd(c);
		if (c.enabled && e - s >= kMinCutSec && t >= s && t < e - 0.02) {
			Seek(e);
			return;
		}
	}
}

void YouTubeCutEditor::SelectCut(int index, bool seek)
{
	if (index < 0 || index >= cuts.size()) {
		return;
	}
	selected = index;
	timeline->SetSelected(index);
	if (table->currentRow() != index) {
		table->selectRow(index);
	}
	RefreshSelectedInfo();
	if (seek) {
		/* Start a little before the cut so you hear how it lands */
		Seek(timeline->EffectiveStart(cuts[index]) - 2.0);
	}
}

void YouTubeCutEditor::StepCut(int direction)
{
	if (cuts.isEmpty()) {
		return;
	}
	double t = Position();
	int target = -1;
	if (direction > 0) {
		for (int i = 0; i < cuts.size(); i++) {
			if (timeline->EffectiveStart(cuts[i]) - 2.0 > t + 0.1) {
				target = i;
				break;
			}
		}
	} else {
		for (int i = int(cuts.size()) - 1; i >= 0; i--) {
			if (timeline->EffectiveStart(cuts[i]) - 2.0 < t - 0.5) {
				target = i;
				break;
			}
		}
	}
	if (target >= 0) {
		SelectCut(target, true);
	}
}

void YouTubeCutEditor::ToggleSelectedCut()
{
	if (selected < 0) {
		return;
	}
	cuts[selected].enabled = !cuts[selected].enabled;
	RefreshRow(selected);
	RefreshSelectedInfo();
	RefreshLength();
	timeline->update();
}

void YouTubeCutEditor::ResetSelectedCut()
{
	if (selected < 0) {
		return;
	}
	YouTubeCut &c = cuts[selected];
	c.start = c.origStart;
	c.end = c.origEnd;
	c.enabled = true;
	RefreshRow(selected);
	RefreshSelectedInfo();
	RefreshLength();
	timeline->update();
}

void YouTubeCutEditor::OnDialsChanged()
{
	if (updating || selected < 0) {
		return;
	}
	YouTubeCut &c = cuts[selected];
	c.start = c.origStart + startDial->value() * kDialStepSec;
	c.end = c.origEnd + endDial->value() * kDialStepSec;
	ClampCut(selected);
	RefreshRow(selected);
	RefreshSelectedInfo();
	RefreshLength();
	timeline->update();
}

void YouTubeCutEditor::OnGlobalDialChanged()
{
	double offset = globalDial->value() * kDialStepSec;
	globalDialLabel->setText(offset == 0.0 ? "As detected"
					       : QString("%1 each side (%2)")
							 .arg(FormatSigned(offset))
							 .arg(offset > 0 ? "cut more" : "keep more"));
	if (updating) {
		return;
	}
	timeline->SetGlobalOffset(offset);
	for (int i = 0; i < cuts.size(); i++) {
		RefreshRow(i);
	}
	RefreshSelectedInfo();
	RefreshLength();
}

void YouTubeCutEditor::ClampCut(int index)
{
	/* A cut stays between its neighbours and inside the video */
	YouTubeCut &c = cuts[index];
	double lo = index > 0 ? cuts[index - 1].end : 0.0;
	double hi = index + 1 < cuts.size() ? cuts[index + 1].start : duration;
	c.start = std::clamp(c.start, lo, hi);
	c.end = std::clamp(c.end, lo, hi);
	if (c.end < c.start) {
		std::swap(c.start, c.end);
	}
}

void YouTubeCutEditor::RefreshTable()
{
	updating = true;
	table->setRowCount(int(cuts.size()));
	for (int row = 0; row < cuts.size(); row++) {
		for (int col = 0; col < table->columnCount(); col++) {
			if (!table->item(row, col)) {
				table->setItem(row, col, new QTableWidgetItem());
			}
		}
		RefreshRow(row);
	}
	updating = false;
}

void YouTubeCutEditor::RefreshRow(int row)
{
	if (row < 0 || row >= cuts.size() || !table->item(row, ColReason)) {
		return;
	}
	const YouTubeCut &c = cuts[row];
	double s = timeline->EffectiveStart(c);
	double e = timeline->EffectiveEnd(c);
	bool active = c.enabled && e - s >= kMinCutSec;
	table->item(row, ColCut)->setText(active ? "✂ Cut" : "↺ Kept");
	table->item(row, ColCut)->setToolTip("Click to undo or redo this cut");
	table->item(row, ColKind)->setText(c.kind == "retake" ? "Retake" : "Dead air");
	table->item(row, ColStart)->setText(FormatTime(s));
	table->item(row, ColEnd)->setText(FormatTime(e));
	table->item(row, ColLength)->setText(QString("%1s").arg(e - s, 0, 'f', 2));
	table->item(row, ColReason)->setText(c.reason);
	QColor color = !active ? QColor(140, 140, 150) : c.kind == "retake" ? QColor(240, 170, 90) : QColor(235, 120, 125);
	for (int col = 0; col < table->columnCount(); col++) {
		table->item(row, col)->setForeground(color);
	}
}

void YouTubeCutEditor::RefreshSelectedInfo()
{
	bool has = selected >= 0 && selected < cuts.size();
	startDial->setEnabled(has);
	endDial->setEnabled(has);
	if (!has) {
		startDialLabel->setText("—");
		endDialLabel->setText("—");
		return;
	}
	const YouTubeCut &c = cuts[selected];
	updating = true;
	startDial->setValue(int(std::lround((c.start - c.origStart) / kDialStepSec)));
	endDial->setValue(int(std::lround((c.end - c.origEnd) / kDialStepSec)));
	updating = false;

	double s = timeline->EffectiveStart(c);
	double e = timeline->EffectiveEnd(c);
	cutInfoLabel->setText(QString("%1 %2 → %3, %4s (detected %5s)%6\n%7")
				      .arg(c.kind == "retake" ? "Retake" : "Dead air")
				      .arg(FormatTime(s), FormatTime(e))
				      .arg(e - s, 0, 'f', 2)
				      .arg(c.origEnd - c.origStart, 0, 'f', 2)
				      .arg(c.enabled ? "" : " — undone, this part is kept")
				      .arg(c.reason));
	startDialLabel->setText(QString("%1\n(left = cut earlier)").arg(FormatSigned(c.start - c.origStart)));
	endDialLabel->setText(QString("%1\n(right = cut later)").arg(FormatSigned(c.end - c.origEnd)));
}

void YouTubeCutEditor::RefreshLength()
{
	/* Merge the effective cuts, since "All cuts" can make neighbours overlap */
	QVector<QPair<double, double>> spans;
	for (const YouTubeCut &c : cuts) {
		double s = timeline->EffectiveStart(c);
		double e = timeline->EffectiveEnd(c);
		if (c.enabled && e - s >= kMinCutSec) {
			spans.append({s, e});
		}
	}
	std::sort(spans.begin(), spans.end());
	double removed = 0.0;
	double cursor = 0.0;
	for (const auto &span : spans) {
		double s = std::max(span.first, cursor);
		if (span.second > s) {
			removed += span.second - s;
			cursor = span.second;
		}
	}
	lengthLabel->setText(QString("Edited length %1 (−%2)").arg(FormatTime(duration - removed), FormatTime(removed)));
}

bool YouTubeCutEditor::Save()
{
	QFile in(cutsPath);
	if (!in.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(this, "Cut Editor", QString("Could not read %1").arg(cutsPath));
		return false;
	}
	QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
	in.close();

	QJsonArray out;
	for (const YouTubeCut &c : cuts) {
		double s = timeline->EffectiveStart(c);
		double e = timeline->EffectiveEnd(c);
		QJsonObject o;
		o["id"] = c.id;
		o["kind"] = c.kind;
		o["reason"] = c.reason;
		o["start"] = std::round(s * 1000.0) / 1000.0;
		o["end"] = std::round(e * 1000.0) / 1000.0;
		o["orig_start"] = c.origStart;
		o["orig_end"] = c.origEnd;
		o["enabled"] = c.enabled && e - s >= kMinCutSec;
		out.append(o);
	}
	root["cuts"] = out;
	root["reviewed"] = true;

	QSaveFile file(cutsPath);
	if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson()) < 0 || !file.commit()) {
		QMessageBox::warning(this, "Cut Editor", QString("Could not save %1").arg(cutsPath));
		return false;
	}
	return true;
}
