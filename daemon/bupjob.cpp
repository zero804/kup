// SPDX-FileCopyrightText: 2020 Simon Persson <simon.persson@mykolab.com>
//
// SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "bupjob.h"

#include <csignal>

#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>

#include <KLocalizedString>

BupJob::BupJob(BackupPlan &pBackupPlan, const QString &pDestinationPath, const QString &pLogFilePath, KupDaemon *pKupDaemon)
   :BackupJob(pBackupPlan, pDestinationPath, pLogFilePath, pKupDaemon)
{
	mFsckProcess.setOutputChannelMode(KProcess::SeparateChannels);
	mIndexProcess.setOutputChannelMode(KProcess::SeparateChannels);
	mSaveProcess.setOutputChannelMode(KProcess::SeparateChannels);
	mPar2Process.setOutputChannelMode(KProcess::SeparateChannels);
	setCapabilities(KJob::Suspendable);
	mHarmlessErrorCount = 0;
	mAllErrorsHarmless = false;
}

void BupJob::performJob() {
	KProcess lPar2Process;
	lPar2Process.setOutputChannelMode(KProcess::SeparateChannels);
	lPar2Process << QStringLiteral("bup") << QStringLiteral("fsck") << QStringLiteral("--par2-ok");
	int lExitCode = lPar2Process.execute();
	if(lExitCode < 0) {
		jobFinishedError(ErrorWithoutLog, xi18nc("@info notification",
		                                         "The <application>bup</application> program is "
		                                         "needed but could not be found, maybe it is not installed?"));
		return;
	}
	if(mBackupPlan.mGenerateRecoveryInfo && lExitCode != 0) {
		jobFinishedError(ErrorWithoutLog, xi18nc("@info notification",
		                                         "The <application>par2</application> program is "
		                                         "needed but could not be found, maybe it is not installed?"));
		return;
	}

	mLogStream << QStringLiteral("Kup is starting bup backup job at ")
	           << QLocale().toString(QDateTime::currentDateTime())
	           << endl << endl;

	KProcess lInitProcess;
	lInitProcess.setOutputChannelMode(KProcess::SeparateChannels);
	lInitProcess << QStringLiteral("bup");
	lInitProcess << QStringLiteral("-d") << mDestinationPath;
	lInitProcess << QStringLiteral("init");
	mLogStream << quoteArgs(lInitProcess.program()) << endl;
	if(lInitProcess.execute() != 0) {
		mLogStream << QString::fromUtf8(lInitProcess.readAllStandardError()) << endl;
		mLogStream << QStringLiteral("Kup did not successfully complete the bup backup job: "
		                             "failed to initialize backup destination.") << endl;
		jobFinishedError(ErrorWithLog, xi18nc("@info notification", "Backup destination could not be initialised. "
		                                                            "See log file for more details."));
		return;
	}

	if(mBackupPlan.mCheckBackups) {
		mFsckProcess << QStringLiteral("bup");
		mFsckProcess << QStringLiteral("-d") << mDestinationPath;
		mFsckProcess << QStringLiteral("fsck") << QStringLiteral("--quick");
		mFsckProcess << QStringLiteral("-j") << QString::number(qMin(4, QThread::idealThreadCount()));

		connect(&mFsckProcess, SIGNAL(finished(int,QProcess::ExitStatus)), SLOT(slotCheckingDone(int,QProcess::ExitStatus)));
		connect(&mFsckProcess, SIGNAL(started()), SLOT(slotCheckingStarted()));
		mLogStream << quoteArgs(mFsckProcess.program()) << endl;
		mFsckProcess.start();
		mInfoRateLimiter.start();
	} else {
		slotCheckingDone(0, QProcess::NormalExit);
	}
}

void BupJob::slotCheckingStarted() {
	makeNice(mFsckProcess.pid());
	emit description(this, i18n("Checking backup integrity"));
}

void BupJob::slotCheckingDone(int pExitCode, QProcess::ExitStatus pExitStatus) {
	QString lErrors = QString::fromUtf8(mFsckProcess.readAllStandardError());
	if(!lErrors.isEmpty()) {
		mLogStream << lErrors << endl;
	}
	mLogStream << "Exit code: " << pExitCode << endl;
	if(pExitStatus != QProcess::NormalExit || pExitCode != 0) {
		mLogStream << QStringLiteral("Kup did not successfully complete the bup backup job: "
		                             "failed integrity check. Your backups could be "
		                             "corrupted! See above for details.") << endl;
		if(mBackupPlan.mGenerateRecoveryInfo) {
			jobFinishedError(ErrorSuggestRepair, xi18nc("@info notification",
			                                            "Failed backup integrity check. Your backups could be corrupted! "
			                                            "See log file for more details. Do you want to try repairing the backup files?"));
		} else {
			jobFinishedError(ErrorWithLog, xi18nc("@info notification",
			                                      "Failed backup integrity check. Your backups could be corrupted! "
			                                      "See log file for more details."));
		}
		return;
	}
	mIndexProcess << QStringLiteral("bup");
	mIndexProcess << QStringLiteral("-d") << mDestinationPath;
	mIndexProcess << QStringLiteral("index") << QStringLiteral("-u");

	foreach(QString lExclude, mBackupPlan.mPathsExcluded) {
		mIndexProcess << QStringLiteral("--exclude");
		mIndexProcess << lExclude;
	}
	QString lExcludesPath = mBackupPlan.absoluteExcludesFilePath();
	if(mBackupPlan.mExcludePatterns && QFileInfo::exists(lExcludesPath)) {
		mIndexProcess << QStringLiteral("--exclude-rx-from") << lExcludesPath;
	}
	mIndexProcess << mBackupPlan.mPathsIncluded;

	connect(&mIndexProcess, SIGNAL(finished(int,QProcess::ExitStatus)), SLOT(slotIndexingDone(int,QProcess::ExitStatus)));
	connect(&mIndexProcess, SIGNAL(started()), SLOT(slotIndexingStarted()));
	mLogStream << quoteArgs(mIndexProcess.program()) << endl;
	mIndexProcess.start();
}

void BupJob::slotIndexingStarted() {
	makeNice(mIndexProcess.pid());
	emit description(this, i18n("Checking what to copy"));
}

void BupJob::slotIndexingDone(int pExitCode, QProcess::ExitStatus pExitStatus) {
	QString lErrors = QString::fromUtf8(mIndexProcess.readAllStandardError());
	if(!lErrors.isEmpty()) {
		mLogStream << lErrors << endl;
	}
	mLogStream << "Exit code: " << pExitCode << endl;
	if(pExitStatus != QProcess::NormalExit || pExitCode != 0) {
		mLogStream << QStringLiteral("Kup did not successfully complete the bup backup job: failed to index everything.") << endl;
		jobFinishedError(ErrorWithLog, xi18nc("@info notification", "Failed to analyze files. "
		                                                            "See log file for more details."));
		return;
	}
	mSaveProcess << QStringLiteral("bup");
	mSaveProcess << QStringLiteral("-d") << mDestinationPath;
	mSaveProcess << QStringLiteral("save");
	mSaveProcess << QStringLiteral("-n") << QStringLiteral("kup") << QStringLiteral("-vv");
	mSaveProcess << mBackupPlan.mPathsIncluded;
	mLogStream << quoteArgs(mSaveProcess.program()) << endl;

	connect(&mSaveProcess, SIGNAL(finished(int,QProcess::ExitStatus)), SLOT(slotSavingDone(int,QProcess::ExitStatus)));
	connect(&mSaveProcess, SIGNAL(started()), SLOT(slotSavingStarted()));
	connect(&mSaveProcess, &KProcess::readyReadStandardError, this, &BupJob::slotReadBupErrors);

	mSaveProcess.setEnv(QStringLiteral("BUP_FORCE_TTY"), QStringLiteral("2"));
	mSaveProcess.start();
}

void BupJob::slotSavingStarted() {
	makeNice(mSaveProcess.pid());
	emit description(this, i18n("Saving backup"));
}

void BupJob::slotSavingDone(int pExitCode, QProcess::ExitStatus pExitStatus) {
	slotReadBupErrors();
	mLogStream << "Exit code: " << pExitCode << endl;
	if(pExitStatus != QProcess::NormalExit || pExitCode != 0) {
		if(mAllErrorsHarmless) {
			mLogStream << QStringLiteral("Only harmless errors detected by Kup.") << endl;
		} else {
			mLogStream << QStringLiteral("Kup did not successfully complete the bup backup job: "
			                             "failed to save everything.") << endl;
			jobFinishedError(ErrorWithLog, xi18nc("@info notification", "Failed to save backup. "
			                                                            "See log file for more details."));
			return;
		}
	}
	if(mBackupPlan.mGenerateRecoveryInfo) {
		mPar2Process << QStringLiteral("bup");
		mPar2Process << QStringLiteral("-d") << mDestinationPath;
		mPar2Process << QStringLiteral("fsck") << QStringLiteral("-g");
		mPar2Process << QStringLiteral("-j") << QString::number(qMin(4, QThread::idealThreadCount()));

		connect(&mPar2Process, SIGNAL(finished(int,QProcess::ExitStatus)), SLOT(slotRecoveryInfoDone(int,QProcess::ExitStatus)));
		connect(&mPar2Process, SIGNAL(started()), SLOT(slotRecoveryInfoStarted()));
		mLogStream << quoteArgs(mPar2Process.program()) << endl;
		mPar2Process.start();
	} else {
		mLogStream << QStringLiteral("Kup successfully completed the bup backup job at ")
		           << QLocale().toString(QDateTime::currentDateTime()) << endl;
		jobFinishedSuccess();
	}
}

void BupJob::slotRecoveryInfoStarted() {
	makeNice(mPar2Process.pid());
	emit description(this, i18n("Generating recovery information"));
}

void BupJob::slotRecoveryInfoDone(int pExitCode, QProcess::ExitStatus pExitStatus) {
	QString lErrors = QString::fromUtf8(mPar2Process.readAllStandardError());
	if(!lErrors.isEmpty()) {
		mLogStream << lErrors << endl;
	}
	mLogStream << "Exit code: " << pExitCode << endl;
	if(pExitStatus != QProcess::NormalExit || pExitCode != 0) {
		mLogStream << QStringLiteral("Kup did not successfully complete the bup backup job: "
		                             "failed to generate recovery info.") << endl;
		jobFinishedError(ErrorWithLog, xi18nc("@info notification", "Failed to generate recovery info for the backup. "
		                                                            "See log file for more details."));
	} else {
		mLogStream << QStringLiteral("Kup successfully completed the bup backup job.") << endl;
		jobFinishedSuccess();
	}
}

void BupJob::slotReadBupErrors() {
	bool lValidInfo = false, lValidFileName = false;
	qulonglong lCopiedKBytes = 0, lTotalKBytes = 0, lCopiedFiles = 0, lTotalFiles = 0;
	ulong lSpeedKBps = 0, lPercent = 0;
	QString lFileName;
	QRegularExpression lProgressRegExp(QStringLiteral("(\\d+)/(\\d+)k, (\\d+)/(\\d+) files\\) \\S* (?:(\\d+)k/s|)"));
	QRegularExpression lErrorCountRegExp(QStringLiteral("WARNING: (\\d+) errors encountered while saving."));

	QTextStream lStream(mSaveProcess.readAllStandardError());
	QString lRawLine;
	while(lStream.readLineInto(&lRawLine)) {
		const QStringList lList = lRawLine.split(QChar::CarriageReturn);
		for(const QString &lLine: lList) {
			//		mLogStream << "read a line: " << lLine << endl;
			if(lLine.startsWith(QStringLiteral("Reading index:")) || lLine.startsWith(QStringLiteral("bloom:"))
			        || lLine.startsWith(QStringLiteral("midx:"))) {
				continue;
			}
			if(lLine.startsWith(QStringLiteral("Saving:"))) {
				QRegularExpressionMatch lMatch = lProgressRegExp.match(lLine);
				if(lMatch.hasMatch()) {
					lCopiedKBytes = lMatch.captured(1).toULongLong();
					lTotalKBytes = lMatch.captured(2).toULongLong();
					lCopiedFiles = lMatch.captured(3).toULongLong();
					lTotalFiles = lMatch.captured(4).toULongLong();
					lSpeedKBps = lMatch.captured(5).toULong();
					if(lTotalKBytes != 0) {
						lPercent = qMax(100*lCopiedKBytes/lTotalKBytes, static_cast<qulonglong>(1));
					}
					lValidInfo = true;
				}
			} else if(lLine.startsWith(QStringLiteral("[Errno 2]"))) {
				mHarmlessErrorCount++;
				mLogStream << lLine << endl;
			} else if(lLine.startsWith(QStringLiteral("WARNING:"))) {
				QRegularExpressionMatch lMatch = lErrorCountRegExp.match(lLine);
				if(lMatch.hasMatch()) {
					int lTotalErrors = lMatch.captured(1).toInt();
					mAllErrorsHarmless = lTotalErrors == mHarmlessErrorCount;
				}
				mLogStream << lLine << endl;
			} else if((lLine.at(0) == ' ' || lLine.at(0) == 'A' || lLine.at(0) == 'M') && lLine.at(1) == ' ' && lLine.at(2) == '/') {
				lFileName = lLine;
				lFileName.remove(0, 2);
				lValidFileName = true;
			} else if(!lLine.startsWith(QStringLiteral("D /"))) {
				mLogStream << lLine << endl;
			}
		}
	}
	if(mInfoRateLimiter.hasExpired(200)) {
		if(lValidInfo) {
			setPercent(lPercent);
			setTotalAmount(KJob::Bytes, lTotalKBytes*1024);
			setTotalAmount(KJob::Files, lTotalFiles);
			setProcessedAmount(KJob::Bytes, lCopiedKBytes*1024);
			setProcessedAmount(KJob::Files, lCopiedFiles);
			emitSpeed(lSpeedKBps * 1024);
		}
		if(lValidFileName) {
			emit description(this, i18n("Saving backup"),
			                 qMakePair(i18nc("Label for file currently being copied", "File"), lFileName));
		}
		mInfoRateLimiter.start();
	}
}

bool BupJob::doSuspend() {
	if(mFsckProcess.state() == KProcess::Running) {
		return 0 == ::kill(mFsckProcess.pid(), SIGSTOP);
	}
	if(mIndexProcess.state() == KProcess::Running) {
		return 0 == ::kill(mIndexProcess.pid(), SIGSTOP);
	}
	if(mSaveProcess.state() == KProcess::Running) {
		return 0 == ::kill(mSaveProcess.pid(), SIGSTOP);
	}
	if(mPar2Process.state() == KProcess::Running) {
		return 0 == ::kill(mPar2Process.pid(), SIGSTOP);
	}
	return false;
}

bool BupJob::doResume() {
	if(mFsckProcess.state() == KProcess::Running) {
		return 0 == ::kill(mFsckProcess.pid(), SIGCONT);
	}
	if(mIndexProcess.state() == KProcess::Running) {
		return 0 == ::kill(mIndexProcess.pid(), SIGCONT);
	}
	if(mSaveProcess.state() == KProcess::Running) {
		return 0 == ::kill(mSaveProcess.pid(), SIGCONT);
	}
	if(mPar2Process.state() == KProcess::Running) {
		return 0 == ::kill(mPar2Process.pid(), SIGCONT);
	}
	return false;
}
