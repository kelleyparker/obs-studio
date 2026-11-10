#include "YouTubeBrollReview.hpp"
#include "YouTubeHeartbeat.hpp"
#include "YouTubeSettingsWidget.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

/* Confidence at which a screen suggestion starts out accepted; matches the
 * pipeline's BROLL_ACCEPT_CONFIDENCE for unreviewed runs. */
static constexpr double kPreAcceptConfidence = 0.6;
/* The dial: about one picture per this many seconds of the edit, as BROLL_AMOUNT_EVERY_SEC */
static constexpr double kAmountEverySec[] = {0, 120, 90, 60, 40, 25};
/* A cloud picture takes about this long (Stable Diffusion's own timings are measured) */
static constexpr double kCloudPictureSec = 15.0;

static QString FormatTime(double sec)
{
	int s = int(sec + 0.5);
	return QString("%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

static QJsonObject ReadPlan(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

YouTubeBrollReview::YouTubeBrollReview(QWidget *parent) : QGroupBox("🎞 B-roll Review", parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setSpacing(6);

	summaryLabel = new QLabel(this);
	summaryLabel->setWordWrap(true);
	summaryLabel->setStyleSheet("color: #ffcc44; font-weight: bold; font-size: 11px;");
	layout->addWidget(summaryLabel);

	amountRow = new QWidget(this);
	QHBoxLayout *amountLayout = new QHBoxLayout(amountRow);
	amountLayout->setContentsMargins(0, 0, 0, 0);
	amountLayout->addWidget(new QLabel("Pictures: Fewer", amountRow));
	amountSlider = new QSlider(Qt::Horizontal, amountRow);
	amountSlider->setRange(0, YouTubeSettingsWidget::kBrollAmountMax);
	amountSlider->setPageStep(1);
	amountSlider->setTickPosition(QSlider::TicksBelow);
	amountSlider->setToolTip("Ticks the most useful picture spots, spread over the video. You can still tick or "
				 "untick any spot yourself. Only this video changes, not Settings.");
	amountLayout->addWidget(amountSlider, 1);
	amountLayout->addWidget(new QLabel("More", amountRow));
	amountName = new QLabel(amountRow);
	amountName->setMinimumWidth(60);
	amountLayout->addWidget(amountName);
	layout->addWidget(amountRow);

	costLabel = new QLabel(this);
	costLabel->setWordWrap(true);
	costLabel->setStyleSheet("color: #b8a8c8; font-size: 11px;");
	layout->addWidget(costLabel);

	QWidget *rowsHost = new QWidget(this);
	rowsLayout = new QVBoxLayout(rowsHost);
	rowsLayout->setContentsMargins(0, 0, 0, 0);
	rowsLayout->setSpacing(6);
	layout->addWidget(rowsHost);

	busyLabel = new QLabel("🖼 Making pictures… progress shows on the B-roll step above. ⏹ Cancel stops it; "
			       "the job keeps waiting here.",
			       this);
	busyLabel->setWordWrap(true);
	busyLabel->setStyleSheet("color: #ffcc44; font-size: 11px;");
	busyLabel->hide();
	layout->addWidget(busyLabel);

	QHBoxLayout *styleRow = new QHBoxLayout();
	styleRow->addWidget(new QLabel("Style:", this));
	styleCombo = new QComboBox(this);
	styleCombo->addItem("B-roll with webcam bubble (PiP)", "pip");
	styleCombo->addItem("Full-screen cutaway", "cutaway");
	styleRow->addWidget(styleCombo, 1);
	layout->addLayout(styleRow);

	makeAllBtn = new QPushButton(this);
	makeAllBtn->setToolTip(
		"Makes a picture for every ticked spot that doesn't have one yet. The job keeps "
		"waiting here, so you can look at them, change a prompt and remake any before rendering.");
	layout->addWidget(makeAllBtn);

	QHBoxLayout *btnRow = new QHBoxLayout();
	renderBtn = new QPushButton("🎬 Render && Upload", this);
	renderBtn->setStyleSheet("QPushButton { font-weight: bold; }");
	new YouTubeHeartbeat(renderBtn);
	skipBtn = new QPushButton("Skip B-roll", this);
	btnRow->addWidget(renderBtn, 1);
	btnRow->addWidget(skipBtn);
	layout->addLayout(btnRow);

	connect(amountSlider, &QSlider::valueChanged, this, [this](int value) {
		amountName->setText(YouTubeSettingsWidget::BrollAmountName(value));
		PickSpots(value);
	});
	connect(makeAllBtn, &QPushButton::clicked, this, [this]() { MakeImages(PicturesToMake()); });
	connect(renderBtn, &QPushButton::clicked, this, [this]() { Finish(false); });
	connect(skipBtn, &QPushButton::clicked, this, [this]() { Finish(true); });

	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	connect(mgr, &YouTubeWorkflowManager::BrollImagesStarted, this, [this]() { SetBusy(true); });
	connect(mgr, &YouTubeWorkflowManager::BrollImagesFinished, this, [this]() { SetBusy(false); });

	hide();
}

void YouTubeBrollReview::ClearRows()
{
	rows.clear();
	while (QLayoutItem *item = rowsLayout->takeAt(0)) {
		if (item->widget()) {
			item->widget()->deleteLater();
		}
		delete item;
	}
}

void YouTubeBrollReview::LoadPlan(const QString &planPath_, const QString &statePath_)
{
	planPath = planPath_;
	statePath = statePath_;
	ClearRows();

	QJsonObject plan = ReadPlan(planPath);
	QJsonArray inserts = plan.value("inserts").toArray();
	int styleIdx = styleCombo->findData(plan.value("style").toString("pip"));
	styleCombo->setCurrentIndex(styleIdx >= 0 ? styleIdx : 0);
	editedTotal = plan.value("edited_total").toDouble();
	maxCoverage = plan.value("max_coverage").toDouble(0.4);

	int screens = 0, pictures = 0;
	for (const QJsonValue &v : inserts) {
		QJsonObject ins = v.toObject();
		InsertRow row;
		row.id = ins.value("id").toString();
		row.picture = ins.value("kind").toString() == "image";
		row.confidence = ins.value("confidence").toDouble();
		row.editedStart = ins.value("edited_start").toDouble();
		row.editedEnd = ins.value("edited_end").toDouble();
		QString image = ins.value("image").toString();
		row.hasImage = row.picture && !image.isEmpty() && QFileInfo::exists(image);
		(row.picture ? pictures : screens)++;

		QFrame *frame = new QFrame(this);
		frame->setStyleSheet("QFrame { background-color: #1d1528; border-radius: 4px; }");
		QHBoxLayout *rowLayout = new QHBoxLayout(frame);
		rowLayout->setContentsMargins(6, 6, 6, 6);

		QLabel *thumb = new QLabel(frame);
		thumb->setFixedSize(160, 90);
		thumb->setAlignment(Qt::AlignCenter);
		thumb->setWordWrap(true);
		QPixmap pix(ins.value("thumb").toString());
		if (!pix.isNull()) {
			thumb->setPixmap(pix.scaled(thumb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
		} else {
			thumb->setText(row.picture ? "Not made yet" : "No preview");
			thumb->setStyleSheet("color: #8a7a9a; border: 1px dashed #4a3a5a;");
		}
		rowLayout->addWidget(thumb, 0, Qt::AlignTop);

		QVBoxLayout *info = new QVBoxLayout();
		row.accept = new QCheckBox(QString("%1 %2 – %3  (%4s)")
						   .arg(row.picture ? "🖼 Picture" : "🖥 Screen",
							FormatTime(row.editedStart), FormatTime(row.editedEnd))
						   .arg(row.editedEnd - row.editedStart, 0, 'f', 1),
					   frame);
		QString status = ins.value("status").toString();
		bool startsTicked = row.picture ? ins.value("picked").toBool() : row.confidence >= kPreAcceptConfidence;
		row.accept->setChecked(status == "accepted" || (status != "rejected" && startsTicked));
		row.accept->setStyleSheet("font-weight: bold;");
		info->addWidget(row.accept);

		QLabel *quote = new QLabel(QString("“%1”").arg(ins.value("quote").toString()), frame);
		quote->setWordWrap(true);
		quote->setStyleSheet("font-style: italic;");
		info->addWidget(quote);

		QString madeBy = ins.value("made_by").toString();
		QLabel *reason = new QLabel(QString("%1 · value %2%3")
						    .arg(ins.value("reason").toString())
						    .arg(row.confidence, 0, 'f', 2)
						    .arg(row.hasImage && !madeBy.isEmpty() ? " · made with " + madeBy
											   : QString()),
					    frame);
		reason->setWordWrap(true);
		reason->setStyleSheet("color: #b8a8c8; font-size: 11px;");
		info->addWidget(reason);

		if (row.picture) {
			row.prompt = new QLineEdit(ins.value("prompt").toString(), frame);
			row.prompt->setPlaceholderText("What the picture shows");
			row.prompt->setToolTip("What the picture shows. Change it, then Remake. Only this text is sent "
					       "to a cloud model, never your video.");
			info->addWidget(row.prompt);

			QHBoxLayout *makeRow = new QHBoxLayout();
			makeRow->addWidget(new QLabel("Make with:", frame));
			row.source = new QComboBox(frame);
			YouTubeSettingsWidget::FillBrollImageSources(row.source, true);
			row.source->setCurrentIndex(
				std::max(0, row.source->findData(ins.value("source").toString("sd"))));
			makeRow->addWidget(row.source, 1);
			row.makeBtn = new QPushButton(row.hasImage ? "↻ Remake" : "🖼 Make", frame);
			makeRow->addWidget(row.makeBtn);
			info->addLayout(makeRow);
		}

		auto makeShift = [frame](const QString &prefix) {
			QDoubleSpinBox *spin = new QDoubleSpinBox(frame);
			spin->setRange(-5.0, 5.0);
			spin->setSingleStep(0.5);
			spin->setDecimals(1);
			spin->setPrefix(prefix);
			spin->setSuffix(" s");
			return spin;
		};
		QHBoxLayout *nudge = new QHBoxLayout();
		row.startShift = makeShift("Start ");
		row.endShift = makeShift("End ");
		nudge->addWidget(row.startShift);
		nudge->addWidget(row.endShift);
		nudge->addStretch();
		info->addLayout(nudge);

		rowLayout->addLayout(info, 1);
		rowsLayout->addWidget(frame);
		rows.append(row);

		const QString id = row.id;
		connect(row.accept, &QCheckBox::toggled, this, [this]() { UpdateSummary(); });
		if (row.picture) {
			connect(row.source, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
				[this]() { UpdateSummary(); });
			connect(row.makeBtn, &QPushButton::clicked, this, [this, id]() {
				for (const InsertRow &r : rows) {
					if (r.id == id) {
						r.accept->setChecked(true);
						MakeImages({&r});
						return;
					}
				}
			});
		}
	}

	amountRow->setVisible(pictures > 0);
	makeAllBtn->setVisible(pictures > 0);
	{
		QSignalBlocker block(amountSlider);
		amountSlider->setValue(plan.value("amount").toInt(2));
	}
	amountName->setText(YouTubeSettingsWidget::BrollAmountName(amountSlider->value()));

	QStringList found;
	if (screens) {
		found << QString("%1 moment(s) to show your screen").arg(screens);
	}
	if (pictures) {
		found << QString("%1 spot(s) for an AI picture").arg(pictures);
	}
	summaryLabel->setText(
		QString("%1 (%2). Untick any you don't want and nudge the timing if needed.%3 Then render.")
			.arg(found.join(" and "), plan.value("provider").toString("planner"),
			     pictures ? " Make the pictures first to see them." : ""));
	SetBusy(YouTubeWorkflowManager::Instance()->IsMakingBrollImages());
	UpdateSummary();
	show();
}

void YouTubeBrollReview::PickSpots(int amount)
{
	QList<InsertRow *> pictures;
	double screenSec = 0.0;
	for (InsertRow &row : rows) {
		if (row.picture) {
			pictures.append(&row);
		} else if (row.accept->isChecked()) {
			screenSec += row.editedEnd - row.editedStart;
		}
	}
	double every = kAmountEverySec[std::clamp(amount, 0, YouTubeSettingsWidget::kBrollAmountMax)];
	int target = every > 0 && editedTotal > 0 ? std::max(1, int(std::ceil(editedTotal / every))) : 0;
	double budget = std::max(0.0, maxCoverage * editedTotal - screenSec);

	std::stable_sort(pictures.begin(), pictures.end(), [](const InsertRow *a, const InsertRow *b) {
		return a->confidence != b->confidence ? a->confidence > b->confidence : a->editedStart < b->editedStart;
	});
	QList<InsertRow *> chosen;
	double used = 0.0;
	if (target > 0) {
		double gap = editedTotal / target / 2.0;
		for (bool spaced : {true, false}) {
			for (InsertRow *row : pictures) {
				if (chosen.size() >= target) {
					break;
				}
				double length = row->editedEnd - row->editedStart;
				if (chosen.contains(row) || used + length > budget + 1e-6) {
					continue;
				}
				if (spaced && std::any_of(chosen.begin(), chosen.end(), [&](const InsertRow *c) {
					    return std::abs(row->editedStart - c->editedStart) < gap;
				    })) {
					continue;
				}
				chosen.append(row);
				used += length;
			}
		}
	}
	for (InsertRow *row : pictures) {
		QSignalBlocker block(row->accept);
		row->accept->setChecked(chosen.contains(row));
	}
	UpdateSummary();
}

QList<const YouTubeBrollReview::InsertRow *> YouTubeBrollReview::PicturesToMake() const
{
	QList<const InsertRow *> todo;
	for (const InsertRow &row : rows) {
		if (row.picture && !row.hasImage && row.accept->isChecked()) {
			todo.append(&row);
		}
	}
	return todo;
}

void YouTubeBrollReview::UpdateSummary()
{
	/* How long Stable Diffusion took last time on this PC (written by the pipeline) */
	QFile file(QDir::homePath() + "/OBSYouTubeImageGen/timings.json");
	QJsonObject t = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
						       : QJsonObject();
	double load = t.value("load_s").toDouble(15), first = t.value("first_image_s").toDouble(27),
	       image = t.value("image_s").toDouble(10);

	int ticked = 0, ready = 0, local = 0, cloud = 0;
	double cost = 0.0;
	for (const InsertRow &row : rows) {
		if (!row.picture || !row.accept->isChecked()) {
			continue;
		}
		ticked++;
		if (row.hasImage) {
			ready++;
			continue;
		}
		QString source = row.source->currentData().toString();
		double price = YouTubeSettingsWidget::BrollImagePrice(source);
		price > 0 ? cloud++ : local++;
		cost += price;
	}
	int todo = local + cloud;
	double seconds = (local ? load + first + (local - 1) * image : 0.0) + cloud * kCloudPictureSec;
	auto clock = [](double sec) {
		int s = int(std::ceil(sec));
		return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
	};

	QString text;
	if (!ticked) {
		text = "No pictures ticked.";
	} else if (!todo) {
		text = QString("%1 picture(s) ticked, all made.").arg(ticked);
	} else {
		QStringList where;
		if (local) {
			where << QString("%1 on this PC (free)").arg(local);
		}
		if (cloud) {
			where << QString("%1 in the cloud (~$%2)").arg(cloud).arg(cost, 0, 'f', 2);
		}
		text = QString("%1 picture(s) ticked, %2 made · to make: %3 · ~%4")
			       .arg(ticked)
			       .arg(ready)
			       .arg(where.join(", "), clock(seconds));
	}
	costLabel->setText(text);
	makeAllBtn->setText(todo ? QString("🖼 Make images (%1, ~%2%3)")
					    .arg(todo)
					    .arg(clock(seconds))
					    .arg(cost > 0 ? QString(", ~$%1").arg(cost, 0, 'f', 2) : QString())
				 : QString("🖼 Make images"));
	makeAllBtn->setEnabled(!busy && todo > 0);
}

void YouTubeBrollReview::SetBusy(bool busy_)
{
	busy = busy_;
	busyLabel->setVisible(busy);
	amountRow->setEnabled(!busy);
	rowsLayout->parentWidget()->setEnabled(!busy);
	styleCombo->setEnabled(!busy);
	renderBtn->setEnabled(!busy);
	skipBtn->setEnabled(!busy);
	makeAllBtn->setEnabled(!busy && !PicturesToMake().isEmpty());
}

bool YouTubeBrollReview::SavePlan(bool acceptNone)
{
	QJsonObject plan = ReadPlan(planPath);
	if (plan.isEmpty()) {
		return false;
	}

	QJsonArray inserts = plan.value("inserts").toArray();
	for (int i = 0; i < inserts.size(); i++) {
		QJsonObject ins = inserts[i].toObject();
		for (const InsertRow &row : rows) {
			if (row.id != ins.value("id").toString()) {
				continue;
			}
			bool accepted = !acceptNone && row.accept->isChecked();
			ins["status"] = accepted ? "accepted" : "rejected";
			/* Nudges move the raw-recording times; the pipeline re-clips them */
			ins["start"] = ins.value("start").toDouble() + row.startShift->value();
			ins["end"] = ins.value("end").toDouble() + row.endShift->value();
			ins["edited_start"] = ins.value("edited_start").toDouble() + row.startShift->value();
			ins["edited_end"] = ins.value("edited_end").toDouble() + row.endShift->value();
			if (row.picture) {
				ins["prompt"] = row.prompt->text().trimmed();
				ins["source"] = row.source->currentData().toString();
			}
		}
		inserts[i] = ins;
	}
	plan["inserts"] = inserts;
	plan["style"] = styleCombo->currentData().toString();
	plan["amount"] = amountSlider->value();

	QSaveFile file(planPath);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(plan).toJson(QJsonDocument::Indented));
	if (!file.commit()) {
		return false;
	}
	/* The nudges are in the plan now; saving again must not move the spots twice */
	for (InsertRow &row : rows) {
		row.startShift->setValue(0.0);
		row.endShift->setValue(0.0);
	}
	return true;
}

void YouTubeBrollReview::MakeImages(const QList<const InsertRow *> &which)
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	if (which.isEmpty() || busy) {
		return;
	}
	double cost = 0.0;
	int paid = 0;
	for (const InsertRow *row : which) {
		if (row->prompt->text().trimmed().isEmpty()) {
			emit mgr->LogMessageReceived("warning", "Write what the picture should show first.");
			row->prompt->setFocus();
			return;
		}
		double price = YouTubeSettingsWidget::BrollImagePrice(row->source->currentData().toString());
		cost += price;
		paid += price > 0;
	}
	if (paid > 0 &&
	    QMessageBox::question(this, "Make pictures in the cloud?",
				  QString("%1 picture(s) will be made by Google's Nano Banana, about $%2 on your "
					  "Gemini API billing. Only the picture descriptions are sent.\n\nGo ahead?")
					  .arg(paid)
					  .arg(cost, 0, 'f', 2)) != QMessageBox::Yes) {
		return;
	}

	QJsonArray ids;
	QJsonObject prompts, sources;
	for (const InsertRow *row : which) {
		ids.append(row->id);
		prompts[row->id] = row->prompt->text().trimmed();
		sources[row->id] = row->source->currentData().toString();
	}
	if (!SavePlan(false)) {
		emit mgr->LogMessageReceived("error", QString("Could not save the B-roll plan: %1").arg(planPath));
		return;
	}
	QJsonObject options;
	options["ids"] = ids;
	options["prompts"] = prompts;
	options["sources"] = sources;
	mgr->MakeBrollImages(options);
}

void YouTubeBrollReview::Finish(bool acceptNone)
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	QList<const InsertRow *> todo = acceptNone ? QList<const InsertRow *>() : PicturesToMake();
	if (!todo.isEmpty()) {
		QMessageBox ask(QMessageBox::Question, "Pictures not made yet",
				QString("%1 ticked picture(s) haven't been made yet.").arg(todo.size()),
				QMessageBox::Cancel, this);
		QPushButton *make = ask.addButton("Make them first", QMessageBox::AcceptRole);
		QPushButton *without = ask.addButton("Render without them", QMessageBox::DestructiveRole);
		ask.setDefaultButton(make);
		ask.exec();
		if (ask.clickedButton() == make) {
			MakeImages(todo);
			return;
		}
		if (ask.clickedButton() != without) {
			return;
		}
	}
	if (!SavePlan(acceptNone)) {
		emit mgr->LogMessageReceived("error", QString("Could not save the B-roll plan: %1").arg(planPath));
		return;
	}
	renderBtn->setEnabled(false);
	skipBtn->setEnabled(false);
	hide();
	mgr->ResumePipeline(statePath);
}
