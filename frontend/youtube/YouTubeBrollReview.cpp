#include "YouTubeBrollReview.hpp"
#include "YouTubeWorkflowManager.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>

/* Confidence at which a suggestion starts out accepted; matches the
 * pipeline's BROLL_ACCEPT_CONFIDENCE for unreviewed runs. */
static constexpr double kPreAcceptConfidence = 0.6;

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

	rowsLayout = new QVBoxLayout();
	rowsLayout->setSpacing(6);
	layout->addLayout(rowsLayout);

	QHBoxLayout *styleRow = new QHBoxLayout();
	styleRow->addWidget(new QLabel("Style:", this));
	styleCombo = new QComboBox(this);
	styleCombo->addItem("Screen with webcam bubble (PiP)", "pip");
	styleCombo->addItem("Full-screen cutaway", "cutaway");
	styleRow->addWidget(styleCombo, 1);
	layout->addLayout(styleRow);

	QHBoxLayout *btnRow = new QHBoxLayout();
	renderBtn = new QPushButton("🎬 Render && Upload", this);
	renderBtn->setStyleSheet("QPushButton { font-weight: bold; }");
	skipBtn = new QPushButton("Skip B-roll", this);
	btnRow->addWidget(renderBtn, 1);
	btnRow->addWidget(skipBtn);
	layout->addLayout(btnRow);

	connect(renderBtn, &QPushButton::clicked, this, [this]() { Finish(false); });
	connect(skipBtn, &QPushButton::clicked, this, [this]() { Finish(true); });

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

	for (const QJsonValue &v : inserts) {
		QJsonObject ins = v.toObject();
		double confidence = ins.value("confidence").toDouble();
		double editedStart = ins.value("edited_start").toDouble();
		double editedEnd = ins.value("edited_end").toDouble();

		QFrame *frame = new QFrame(this);
		frame->setStyleSheet("QFrame { background-color: #1d1528; border-radius: 4px; }");
		QHBoxLayout *rowLayout = new QHBoxLayout(frame);
		rowLayout->setContentsMargins(6, 6, 6, 6);

		QLabel *thumb = new QLabel(frame);
		thumb->setFixedSize(160, 90);
		thumb->setAlignment(Qt::AlignCenter);
		QPixmap pix(ins.value("thumb").toString());
		if (!pix.isNull()) {
			thumb->setPixmap(pix.scaled(thumb->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
		} else {
			thumb->setText("No preview");
		}
		rowLayout->addWidget(thumb);

		QVBoxLayout *info = new QVBoxLayout();
		QCheckBox *accept = new QCheckBox(QString("%1 – %2  (%3s)")
							  .arg(FormatTime(editedStart), FormatTime(editedEnd))
							  .arg(editedEnd - editedStart, 0, 'f', 1),
						  frame);
		accept->setChecked(confidence >= kPreAcceptConfidence);
		accept->setStyleSheet("font-weight: bold;");
		info->addWidget(accept);

		QLabel *quote = new QLabel(QString("“%1”").arg(ins.value("quote").toString()), frame);
		quote->setWordWrap(true);
		quote->setStyleSheet("font-style: italic;");
		info->addWidget(quote);

		QLabel *reason = new QLabel(
			QString("%1 · confidence %2").arg(ins.value("reason").toString()).arg(confidence, 0, 'f', 2),
			frame);
		reason->setWordWrap(true);
		reason->setStyleSheet("color: #b8a8c8; font-size: 11px;");
		info->addWidget(reason);

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
		QDoubleSpinBox *startShift = makeShift("Start ");
		QDoubleSpinBox *endShift = makeShift("End ");
		nudge->addWidget(startShift);
		nudge->addWidget(endShift);
		nudge->addStretch();
		info->addLayout(nudge);

		rowLayout->addLayout(info, 1);
		rowsLayout->addWidget(frame);
		rows.append(InsertRow{ins.value("id").toString(), accept, startShift, endShift});
	}

	summaryLabel->setText(QString("%1 moment(s) to show your screen (%2). Untick any you don't want, nudge the "
				      "timing if needed, then render.")
				      .arg(inserts.size())
				      .arg(plan.value("provider").toString("planner")));
	renderBtn->setEnabled(true);
	skipBtn->setEnabled(true);
	show();
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
		}
		inserts[i] = ins;
	}
	plan["inserts"] = inserts;
	plan["style"] = styleCombo->currentData().toString();

	QSaveFile file(planPath);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(plan).toJson(QJsonDocument::Indented));
	return file.commit();
}

void YouTubeBrollReview::Finish(bool acceptNone)
{
	YouTubeWorkflowManager *mgr = YouTubeWorkflowManager::Instance();
	if (!SavePlan(acceptNone)) {
		emit mgr->LogMessageReceived("error", QString("Could not save the B-roll plan: %1").arg(planPath));
		return;
	}
	renderBtn->setEnabled(false);
	skipBtn->setEnabled(false);
	hide();
	mgr->ResumePipeline(statePath);
}
