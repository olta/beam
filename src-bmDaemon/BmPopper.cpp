/*
	BmPopper.cpp
		- Implements the main POP3-client-class: BmPopper

		$Id$
*/

#include <memory.h>
#include <memory>
#include <stdio.h>

#include "BmLogHandler.h"
#include "BmMailReceived.h"
#include "BmMsgTypes.h"
#include "BmPopAccount.h"
#include "BmPopper.h"
#include "BmPrefs.h"
#include "BmUtil.h"

// standard logfile-name for this class:
#undef BM_LOGNAME
#define BM_LOGNAME Name()

/*------------------------------------------------------------------------------*\
	PopStates[]
		-	array of POP3-states, each with title and corresponding handler-method
\*------------------------------------------------------------------------------*/
BmPopper::PopState BmPopper::PopStates[BmPopper::POP_FINAL] = {
	PopState( "connect...", &BmPopper::Connect),
	PopState( "login...", &BmPopper::Login),
	PopState( "check...", &BmPopper::Check),
	PopState( "get...", &BmPopper::Retrieve),
	PopState( "quit...", &BmPopper::Disconnect),
	PopState( "done", NULL)
};

/*------------------------------------------------------------------------------*\
	BmPopper( info)
		-	contructor
\*------------------------------------------------------------------------------*/
BmPopper::BmPopper( const BString& name, BmPopAccount* account)
	:	BmJobModel( name)
	,	mPopAccount( account)
	,	mPopServer()
	,	mConnected( false)
	,	mMsgUIDs( NULL)
	,	mMsgCount( 0)
	,	mMsgSize( 0)
	,	mMsgTotalSize( 1)
	,	mState( 0)
{
}

/*------------------------------------------------------------------------------*\
	~BmPopper()
		-	destructor
		-	frees all associated memory (hopefully)
\*------------------------------------------------------------------------------*/
BmPopper::~BmPopper() { 
	if (mConnected) {
		//	We try to inform POP-server about QUIT, if still connected.
		// This probably means that we ran into an exception, so maybe it's not really
		// a good idea...(?)
		this->Quit();
	}
	if (mMsgUIDs)
		delete [] mMsgUIDs;
}

/*------------------------------------------------------------------------------*\
	StartJob()
		-	the mainloop, steps through all POP3-stages and calls the corresponding handlers
\*------------------------------------------------------------------------------*/
void BmPopper::StartJob() {

	const float delta = (100.0 / POP_DONE);
	const bool failed=true;

	mPopServer.InitCheck() == B_OK									||	BM_THROW_RUNTIME("BmPopper: could not create NetEndpoint");
	try {
		for( mState=POP_CONNECT; ShouldContinue() && mState<POP_DONE; ++mState) {
			TStateMethod stateFunc = PopStates[mState].func;
			UpdatePOPStatus( (mState==POP_CONNECT ? 0.0 : delta), NULL);
			(this->*stateFunc)();
		}
		UpdatePOPStatus( delta, NULL);
	}
	catch( BM_runtime_error &err) {
		// a problem occurred, we tell the user:
		BString errstr = err.what();
		int e;
		if ((e = mPopServer.Error()))
			errstr << "\nerror: " << e << ", " << mPopServer.ErrorStr();
		UpdatePOPStatus( 0.0, NULL, failed);
		BString text = Name() << "\n\n" << errstr;
		BM_SHOWERR( BString("BmPopper: ") << text);
	}
}

/*------------------------------------------------------------------------------*\
	UpdatePOPStatus( delta, detailText, failed)
		-	informs the interested party about a change in the current POP3-state
		-	failed==true means that we only want to indicate the failure of the
			current stage (the BString "FAILED!" will be shown)
\*------------------------------------------------------------------------------*/
void BmPopper::UpdatePOPStatus( const float delta, const char* detailText, 
										  bool failed) {
	auto_ptr<BMessage> msg( new BMessage( BM_JOB_UPDATE_STATE));
	msg->AddString( MSG_POPPER, Name().String());
	msg->AddString( BmJobModel::MSG_DOMAIN, "statbar");
	msg->AddFloat( MSG_DELTA, delta);
	if (failed)
		msg->AddString( MSG_LEADING, BString(PopStates[mState].text) << "   FAILED!");
	else
		msg->AddString( MSG_LEADING, PopStates[mState].text);
	if (detailText)
		msg->AddString( MSG_TRAILING, detailText);
	TellControllers( msg.get());
}

/*------------------------------------------------------------------------------*\
	UpdateMailStatus( delta, detailText)
		- informs the interested party about the message currently dealt with
\*------------------------------------------------------------------------------*/
void BmPopper::UpdateMailStatus( const float delta, const char* detailText, 
											int32 currMsg) {
	BString text;
	if (mMsgCount) {
		text = BString() << currMsg << " of " << mMsgCount;
	} else {
		text = "none";
	}
	auto_ptr<BMessage> msg( new BMessage( BM_JOB_UPDATE_STATE));
	msg->AddString( MSG_POPPER, Name().String());
	msg->AddString( BmJobModel::MSG_DOMAIN, "mailbar");
	msg->AddFloat( MSG_DELTA, delta);
	msg->AddString( MSG_LEADING, text);
	if (detailText)
		msg->AddString( MSG_TRAILING, detailText);
	TellControllers( msg.get());
}

/*------------------------------------------------------------------------------*\
	Connect()
		-	Initiates network-connection to POP-server
\*------------------------------------------------------------------------------*/
void BmPopper::Connect() {
	if (mPopServer.Connect( mPopAccount->POPAddress()) != B_OK) {
		BString s = BString("Could not connect to POP-Server ") << mPopAccount->POPServer();
		throw BM_network_error( s);
	}
	mConnected = true;
	CheckForPositiveAnswer( SINGLE_LINE);
}

/*------------------------------------------------------------------------------*\
	Login()
		-	Sends user/passwd combination and checks result
\*------------------------------------------------------------------------------*/
void BmPopper::Login() {
	BString cmd = BString("USER ") << mPopAccount->Username();
	SendCommand( cmd);
	if (CheckForPositiveAnswer( SINGLE_LINE)) {
		cmd = BString("PASS ") << mPopAccount->Password();
		SendCommand( cmd);
		CheckForPositiveAnswer( SINGLE_LINE);
	}
}

/*------------------------------------------------------------------------------*\
	Check()
		-	looks for new mail
\*------------------------------------------------------------------------------*/
void BmPopper::Check() {
	BString cmd("STAT");
	SendCommand( cmd);
	if (!CheckForPositiveAnswer( SINGLE_LINE))
		return;
	if (sscanf( mReplyLine.String()+4, "%ld %ld", &mMsgCount, &mMsgTotalSize) != 2 || mMsgCount < 0)
		throw BM_network_error( "answer to STAT has unknown format");
	if (mMsgCount == 0) {
		UpdateMailStatus( 0, NULL, 0);
		return;									// no messages found, nothing more to do
	}

	// we try to fetch a list of unique message IDs from server:
	mMsgUIDs = new BString[mMsgCount];
	cmd = BString("UIDL");
	SendCommand( cmd);
	// The UIDL-command may not be implemented by this server, so we 
	// do not require a postive answer, we just hope for it:
	if (!GetAnswer( MULTI_LINE)) 			
		return;
	if (mReplyLine[0] == '+') {
		// ok, we've got the UIDL-listing, so we fetch it:
		int32 msgNum;
		char msgUID[71];
		// fetch UIDLs one per line and store them in array:
		const char *p = mAnswer.String();
		for( int32 i=0; i<mMsgCount; i++) {
			if (sscanf( p, "%ld %70s", &msgNum, msgUID) != 2 || msgNum <= 0)
				throw BM_network_error( "answer to UIDL has unknown format");
			mMsgUIDs[i] = msgUID;
			// skip to next line:
			if (!(p = strstr( p, "\r\n")))
				throw BM_network_error( "answer to UIDL has unknown format");
			p += 2;
		}
	} else {
		// no UIDL-listing from server, we will have to fetch the UIDLs later
		// or generate our own (happens in class BmMailReceived)
	}
}

/*------------------------------------------------------------------------------*\
	Retrieve()
		-	retrieves all mails from server
\*------------------------------------------------------------------------------*/
void BmPopper::Retrieve() {
	int32 num;
	for( int32 i=0; i<mMsgCount; i++) {
		BString cmd = BString("LIST ") << i+1;
		SendCommand( cmd);
		if (!CheckForPositiveAnswer( SINGLE_LINE))
			return;
		if (sscanf( mReplyLine.String()+4, "%ld %ld", &num, &mMsgSize) != 2 || num != i+1)
		throw BM_network_error( "answer to LIST has unknown format");
		cmd = BString("RETR ") << i+1;
		SendCommand( cmd);
		if (!CheckForPositiveAnswer( MULTI_LINE, i+1))
			return;
		BmMailReceived mail( mAnswer, mMsgUIDs[i], Name());
		mail.Store();
	}
	if (mMsgCount)
		UpdateMailStatus( 0, "done", mMsgCount);
	//	delete the retrieved messages if required:
	if (mPopAccount->DeleteMailFromServer()) {
		for( int32 i=0; i<mMsgCount; i++) {
			BString cmd = BString("DELE ") << i+1;
			SendCommand( cmd);
			if (!CheckForPositiveAnswer( SINGLE_LINE))
				return;
		}
	}
}

/*------------------------------------------------------------------------------*\
	Disconnect()
		-	tells the server that we are finished
\*------------------------------------------------------------------------------*/
void BmPopper::Disconnect() {
	Quit( true);
}

/*------------------------------------------------------------------------------*\
	Quit( WaitForAnswer)
		-	sends a QUIT to the server, waiting for answer only 
			if WaitForAnswer==true
		-	normally, we wait for an answer, just if we are shutting down
			because of an error we ignore any answer.
		-	the network-connection is always closed
\*------------------------------------------------------------------------------*/
void BmPopper::Quit( bool WaitForAnswer) {
	BString cmd("QUIT");
	try {
		SendCommand( cmd);
		if (WaitForAnswer) {
			GetAnswer( SINGLE_LINE);
		}
	} catch(...) {	}
	mPopServer.Close();
	mConnected = false;
}

/*------------------------------------------------------------------------------*\
	CheckForPositiveAnswer( SingleLineMode, mailNr)
		-	waits for an answer from server and checks if it is positive
		-	throws an exception if answer is negative
		-	parameters are just passed on
\*------------------------------------------------------------------------------*/
bool BmPopper::CheckForPositiveAnswer( bool SingleLineMode, int32 mailNr) {
	if (GetAnswer( SingleLineMode, mailNr)) {
		if (mReplyLine[0] != '+') {
			BString err("Server answers: \n");
			err += mReplyLine;
			err.RemoveAll( "\r");
			throw BM_network_error( err);
		}
		return true;
	} else {
		// user has interrupted
		return false;
	}
}

/*------------------------------------------------------------------------------*\
	GetAnswer( SingleLineMode, mailNr)
		-	waits for an answer from server and stores it in mAnswer
		-	mailNr > 0 if answer is a mail-message, mailNr==0 otherwise
		-	first line of answer (server-reply-line) is saved into mReplyLine.
			Because of this, the reply-line does not appear in mAnswer.
\*------------------------------------------------------------------------------*/
bool BmPopper::GetAnswer( bool SingleLineMode, int32 mailNr) {
	int32 offset = 0;
	int32 SMALL = 512;
	int32 bufSize = (mailNr>0 && mMsgSize > BmPopper::NetBufSize) 
							? mMsgSize+SMALL*4
							: BmPopper::NetBufSize;
	char *buffer;
	bool done = false;
	bool firstBlock = true;
	bool replyLineExtracted = false;
	int32 numBytes = 0;

	if (mailNr)
		BM_LOG3( BM_LogPop, BString("announced msg-size:") << mMsgSize);
	BM_LOG3( BM_LogPop, BString("bufSize:") << bufSize);
	mAnswer.SetTo( '\0', bufSize);		// preallocate the bufsize we need
	buffer = mAnswer.LockBuffer( 0);
	try {
		do {
			int32 bufFree = bufSize - offset;
			if (bufFree < SMALL) {
				// bufsize is not sufficient, we enlarge the buffer:
				bufSize *= 2;
				mAnswer.UnlockBuffer( offset);
				buffer = mAnswer.LockBuffer( bufSize);
				bufFree = bufSize - offset;
				BM_LOG2( BM_LogPop, BString("bufSize enlarged to:") << bufSize);
			}
			if (bufFree > BmPopper::NetBufSize)
				bufFree = BmPopper::NetBufSize;
			numBytes = ReceiveBlock( buffer+offset, bufFree);
			if (!replyLineExtracted) {
				// we may have to extract the reply-line from the buffer:
				char* eol;
				if ((eol=strstr( buffer, "\r\n")) != NULL) {
					// reply-line is complete, we extract it from buffer:
					*eol = 0;
					eol += 2;
					mReplyLine = buffer;
					int32 len = mReplyLine.Length();
					strcpy( buffer, eol);
					offset -= (len+2);
					replyLineExtracted = true;
					BM_LOG( BM_LogPop, BString("<--\n") << mReplyLine);
					if (SingleLineMode || firstBlock && mReplyLine[0]=='-') {
						// if all we expect is the reply-line, or if answer is negative, we are done:
						done = true;
					}
				};
			}
			if (!SingleLineMode) {
				// MULTI_LINE mode
				int32 searchOffset = (offset > 3 ? offset-4 : 0);
				char *endp;
				if ((endp=strstr( buffer+searchOffset, "\r\n.\r\n"))) {
					*(endp+2)='\0';
					// end of multiline-answer is indicated by line consisting only of a dot
					if (!mailNr) {
						BM_LOG2( BM_LogPop, BString("<--\n") << buffer);
					}
					done = true;
				}
			}
			offset += numBytes;
			firstBlock = false;
			if (mailNr > 0) {
				float delta = (100.0 * numBytes) / (mMsgTotalSize ? mMsgTotalSize : 1);
				BString text = BString("size: ") << BytesToString( mMsgSize);
				UpdateMailStatus( delta, text.String(), mailNr);
			}
		} while( !done && numBytes);
		mAnswer.UnlockBuffer( -1);
		if (done) {
			mAnswer.ReplaceAll( "\n..", "\n.");	// remove padding of "termination octet" (dot) inside message
		} else  {
			//	numBytes == 0, interrupt by external event (user)
			mAnswer = "";
		}
	} catch (...) {
		mAnswer.UnlockBuffer( -1);
		throw;
	}
	return done;
}

/*------------------------------------------------------------------------------*\
	ReceiveBlock( buffer, max)
		-	receives a block of a specified size (<=max) from server and appends
			it to mAnswer (well, writes it into mAnswer's buffer)
		-	ensures user-feedback is not blocked longer than BmPopper::FeedbackTimeout
		-	waits only BmPopper::ReceiveTimeout seconds for answer,
			throws an exception if no answer has arrived within that timeframe
		-	returns size of received block in bytes
\*------------------------------------------------------------------------------*/
int32 BmPopper::ReceiveBlock( char* buffer, int32 max) {
	int32 numBytes;
	int32 AnswerTimeout = ThePrefs->ReceiveTimeout()*1000*1000;
	int32 timeout = AnswerTimeout / BmPopper::FeedbackTimeout;
	bool shouldCont;
	for( int32 round=0; (shouldCont = ShouldContinue()) && round<timeout; ++round) {
		if (mPopServer.IsDataPending( BmPopper::FeedbackTimeout)) {
			if ((numBytes = mPopServer.Receive( buffer, max-1)) > 0) {
				buffer[numBytes] = '\0';
				return numBytes;
			} else if (numBytes < 0) {
				throw BM_network_error( "error during receive");
			}
		}
	}
	if (shouldCont) {
		throw BM_network_error( "timeout during receive from POP-server");
	}
	return 0;
}

/*------------------------------------------------------------------------------*\
	SendCommand( cmd)
		-	sends the specified POP3-command to the server.
\*------------------------------------------------------------------------------*/
void BmPopper::SendCommand( BString cmd) {
	cmd << "\r\n";
	int32 size = cmd.Length(), sentSize;
	if (cmd.IFindFirst("PASS") != B_ERROR) {
		BM_LOG( BM_LogPop, "-->\nPASS password_omitted_here");
													// we do not want to log the password...
	} else {
		BM_LOG( BM_LogPop, BString("-->\n") << cmd);
	}
	if ((sentSize = mPopServer.Send( cmd.String(), size)) != size) {
		throw BM_network_error( BString("error during send, sent only ") << sentSize << " bytes instead of " << size);
	}
}

/*------------------------------------------------------------------------------*\
	Initialize statics:
\*------------------------------------------------------------------------------*/
int32 BmPopper::mId = 0;
int32 BmPopper::FeedbackTimeout = 200*1000;

