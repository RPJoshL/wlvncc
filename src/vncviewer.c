/*
 *  Copyright (C) 2024 Andri Yngvason.  All Rights Reserved.
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

/*
 * vncviewer.c - the Xt-based VNC viewer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include "rfbclient.h"
#include "tls.h"

extern const char* tls_cert_path;
extern const char* auth_command;

static void Dummy(rfbClient* client) {
}
static rfbBool DummyPoint(rfbClient* client, int x, int y) {
  return TRUE;
}
static void DummyRect(rfbClient* client, int x, int y, int w, int h) {
}

#include <termios.h>

static char* ReadLine(const char* title)
{
	fprintf(stderr, "%s: ", title);
	fflush(stderr);

	char* line = NULL;
	size_t size = 0;
	ssize_t len = getline(&line, &size, stdin);

	// Trim off the newline character
	if (len > 0)
		line[len - 1] = '\0';

	return line;
}

static char* ReadLineNoEcho(const char* title)
{
	struct termios save, noecho;

	if (tcgetattr(fileno(stdin), &save) != 0)
		return NULL;

	noecho = save;
	noecho.c_lflag &= ~ECHO;

	if (tcsetattr(fileno(stdin), TCSAFLUSH, &noecho) != 0)
		return NULL;

	char* line = ReadLine(title);
	tcsetattr(fileno(stdin), TCSAFLUSH, &save);

	return line;
}

static int RunAuthCommand(char** username, char** password)
{
	int fds[2];
	if (pipe(fds) < 0) {
		rfbClientLog("Failed to create pipe: %m\n");
		return -1;
	}

	int rfd = fds[0];
	int wfd = fds[1];

	FILE* out = fdopen(rfd, "r");
	if (!out) {
		rfbClientLog("Failed to open output stream: %m\n");
		return -1;
	}

	pid_t pid = fork();
	if (pid == -1) {
		rfbClientLog("Failed to fork: %m\n");
		return -1;
	}

	if (pid == 0) {
		int rc = dup2(wfd, STDOUT_FILENO);
		close(wfd);
		if (rc < 0) {
			fprintf(stderr, "Failed to replace stdout after fork\n");
			exit(EXIT_FAILURE);
		}

		const char* shell = getenv("SHELL");
		if (!shell)
			shell = "/bin/sh";

		execl(shell, shell, "-c", auth_command, (char*)NULL);
		_exit(EXIT_FAILURE);
	}

	close(wfd);

	int wstatus;
	int rc = waitpid(pid, &wstatus, 0);
	if (rc < 0) {
		rfbClientLog("Failed to wait for auth script: %m\n");
		goto out;
	}


	if (!WIFEXITED(wstatus)) {
		rfbClientLog("Auth script exited with a failure: %d\n",
				WEXITSTATUS(wstatus));
		goto out;
	}

	char* line = NULL;
	size_t size = 0;

	ssize_t len;

	if (username) {
		len = getline(&line, &size, out);
		if (len > 0) {
			line[len - 1] = '\0';
			*username = strdup(line);
		}
	}

	len = getline(&line, &size, out);
	if (len > 0) {
		line[len - 1] = '\0';
		*password = strdup(line);
	}

	if ((*username && !*username) || !*password) {
		rfbClientLog("Did not get credentials from auth script\n");
		return -1;
	}

out:
	fclose(out);
	return rc;
}

static char* ReadPassword(rfbClient* client) {
	if (auth_command) {
		char* password = NULL;
		if (RunAuthCommand(NULL, &password) < 0)
			return NULL;
		return password;
	}

	return ReadLineNoEcho("Password");
}

static rfbCredential* ReadUsernameAndPassword(rfbClient* client)
{
	rfbCredential *cred = calloc(1, sizeof(*cred));

	if (auth_command) {
		if (RunAuthCommand(&cred->userCredential.username,
					&cred->userCredential.password) < 0)
			goto failure;
		return cred;
	}

  // Try to get credentials from env
  char* username = getenv("VNC_USERNAME");
	char* password =  getenv("VNC_PASSWORD");

  if (username && password) {
    rfbClientLog("Using username and password for VNC authentication 'VNC_USERNAME', 'VNC_PASSWORD'\n");
    cred->userCredential.password = malloc(strlen(password) + 1);
		cred->userCredential.username = malloc(strlen(username) + 1);
		strcpy(cred->userCredential.password, password);
		strcpy(cred->userCredential.username, username);
  } else {
    cred->userCredential.username = ReadLine("User");
    cred->userCredential.password = ReadLineNoEcho("Password");
  }


	if (!cred->userCredential.username || !cred->userCredential.password) {
		goto failure;
	}

	return cred;

failure:
	free(cred->userCredential.username);
	free(cred->userCredential.password);
	free(cred);
	return NULL;
}

static rfbCredential* ReadX509Creds(rfbClient* client)
{
	const char *ca_cert = tls_cert_path ? tls_cert_path : "/etc/ssl/cert.pem";

	if (access(ca_cert, F_OK) != 0) {
		rfbClientLog("Missing CA certificates (%s)\n", ca_cert);
		return NULL;
	}

	rfbCredential *cred = calloc(1, sizeof(*cred));
	cred->x509Credential.x509CACertFile = strdup(ca_cert);

	return cred;
}

static rfbCredential* GetCredentials(rfbClient* client, int type)
{
	switch (type) {
	case rfbCredentialTypeUser: return ReadUsernameAndPassword(client);
	case rfbCredentialTypeX509: return ReadX509Creds(client);
	}
	return NULL;
}

static rfbBool MallocFrameBuffer(rfbClient* client) {
  uint64_t allocSize;

  if(client->frameBuffer) {
    free(client->frameBuffer);
    client->frameBuffer = NULL;
  }

  /* SECURITY: promote 'width' into uint64_t so that the multiplication does not overflow
     'width' and 'height' are 16-bit integers per RFB protocol design
     SIZE_MAX is the maximum value that can fit into size_t
  */
  allocSize = (uint64_t)client->width * client->height * client->format.bitsPerPixel/8;

  if (allocSize >= SIZE_MAX) {
    rfbClientErr("CRITICAL: cannot allocate frameBuffer, requested size is too large\n");
    return FALSE;
  }

  client->frameBuffer=malloc( (size_t)allocSize );

  if (client->frameBuffer == NULL)
    rfbClientErr("CRITICAL: frameBuffer allocation failed, requested size too large or not enough memory?\n");

  return client->frameBuffer?TRUE:FALSE;
}

/* messages */

static rfbBool CheckRect(rfbClient* client, int x, int y, int w, int h) {
  return x + w <= client->width && y + h <= client->height;
}

static void FillRectangle(rfbClient* client, int x, int y, int w, int h, uint32_t colour) {
  int i,j;

  if (client->frameBuffer == NULL) {
      return;
  }

  if (!CheckRect(client, x, y, w, h)) {
    rfbClientLog("Rect out of bounds: %dx%d at (%d, %d)\n", x, y, w, h);
    return;
  }

#define FILL_RECT(BPP) \
    for(j=y*client->width;j<(y+h)*client->width;j+=client->width) \
      for(i=x;i<x+w;i++) \
	((uint##BPP##_t*)client->frameBuffer)[j+i]=colour;

  switch(client->format.bitsPerPixel) {
  case  8: FILL_RECT(8);  break;
  case 16: FILL_RECT(16); break;
  case 32: FILL_RECT(32); break;
  default:
    rfbClientLog("Unsupported bitsPerPixel: %d\n",client->format.bitsPerPixel);
  }
}

static void CopyRectangle(rfbClient* client, const uint8_t* buffer, int x, int y, int w, int h) {
  int j;

  if (client->frameBuffer == NULL) {
      return;
  }

  if (!CheckRect(client, x, y, w, h)) {
    rfbClientLog("Rect out of bounds: %dx%d at (%d, %d)\n", x, y, w, h);
    return;
  }

#define COPY_RECT(BPP) \
  { \
    int rs = w * BPP / 8, rs2 = client->width * BPP / 8; \
    for (j = ((x * (BPP / 8)) + (y * rs2)); j < (y + h) * rs2; j += rs2) { \
      memcpy(client->frameBuffer + j, buffer, rs); \
      buffer += rs; \
    } \
  }

  switch(client->format.bitsPerPixel) {
  case  8: COPY_RECT(8);  break;
  case 16: COPY_RECT(16); break;
  case 32: COPY_RECT(32); break;
  default:
    rfbClientLog("Unsupported bitsPerPixel: %d\n",client->format.bitsPerPixel);
  }
}

/* TODO: test */
static void CopyRectangleFromRectangle(rfbClient* client, int src_x, int src_y, int w, int h, int dest_x, int dest_y) {
  int i,j;

  if (client->frameBuffer == NULL) {
      return;
  }

  if (!CheckRect(client, src_x, src_y, w, h)) {
    rfbClientLog("Source rect out of bounds: %dx%d at (%d, %d)\n", src_x, src_y, w, h);
    return;
  }

  if (!CheckRect(client, dest_x, dest_y, w, h)) {
    rfbClientLog("Dest rect out of bounds: %dx%d at (%d, %d)\n", dest_x, dest_y, w, h);
    return;
  }

#define COPY_RECT_FROM_RECT(BPP) \
  { \
    uint##BPP##_t* _buffer=((uint##BPP##_t*)client->frameBuffer)+(src_y-dest_y)*client->width+src_x-dest_x; \
    if (dest_y < src_y) { \
      for(j = dest_y*client->width; j < (dest_y+h)*client->width; j += client->width) { \
        if (dest_x < src_x) { \
          for(i = dest_x; i < dest_x+w; i++) { \
            ((uint##BPP##_t*)client->frameBuffer)[j+i]=_buffer[j+i]; \
          } \
        } else { \
          for(i = dest_x+w-1; i >= dest_x; i--) { \
            ((uint##BPP##_t*)client->frameBuffer)[j+i]=_buffer[j+i]; \
          } \
        } \
      } \
    } else { \
      for(j = (dest_y+h-1)*client->width; j >= dest_y*client->width; j-=client->width) { \
        if (dest_x < src_x) { \
          for(i = dest_x; i < dest_x+w; i++) { \
            ((uint##BPP##_t*)client->frameBuffer)[j+i]=_buffer[j+i]; \
          } \
        } else { \
          for(i = dest_x+w-1; i >= dest_x; i--) { \
            ((uint##BPP##_t*)client->frameBuffer)[j+i]=_buffer[j+i]; \
          } \
        } \
      } \
    } \
  }

  switch(client->format.bitsPerPixel) {
  case  8: COPY_RECT_FROM_RECT(8);  break;
  case 16: COPY_RECT_FROM_RECT(16); break;
  case 32: COPY_RECT_FROM_RECT(32); break;
  default:
    rfbClientLog("Unsupported bitsPerPixel: %d\n",client->format.bitsPerPixel);
  }
}

static void initAppData(AppData* data) {
	data->shareDesktop=TRUE;
	data->viewOnly=FALSE;
	data->encodingsString="tight zrle ultra copyrect hextile zlib corre rre raw";
	data->useBGR233=FALSE;
	data->nColours=0;
	data->forceOwnCmap=FALSE;
	data->forceTrueColour=FALSE;
	data->requestedDepth=0;
	data->compressLevel=3;
	data->qualityLevel=5;
#ifdef LIBVNCSERVER_HAVE_LIBJPEG
	data->enableJPEG=TRUE;
#else
	data->enableJPEG=FALSE;
#endif
	data->useRemoteCursor=FALSE;
}

rfbClient* rfbGetClient(int bitsPerSample,int samplesPerPixel,
			int bytesPerPixel) {
  rfbClient* client=(rfbClient*)calloc(sizeof(rfbClient),1);
  if(!client) {
    rfbClientErr("Couldn't allocate client structure!\n");
    return NULL;
  }
  initAppData(&client->appData);
  client->endianTest = 1;
  client->programName="";
  client->serverHost=strdup("");
  client->serverPort=5900;

  client->destHost = NULL;
  client->destPort = 5900;
  
  client->connectTimeout = DEFAULT_CONNECT_TIMEOUT;
  client->readTimeout = DEFAULT_READ_TIMEOUT;

  /* default: use complete frame buffer */ 
  client->updateRect.x = -1;
 
  client->frameBuffer = NULL;
  client->outputWindow = 0;
 
  client->format.bitsPerPixel = bytesPerPixel*8;
  client->format.depth = bitsPerSample*samplesPerPixel;
  client->appData.requestedDepth=client->format.depth;
  client->format.bigEndian = *(char *)&client->endianTest?FALSE:TRUE;
  client->format.trueColour = 1;

  if (client->format.bitsPerPixel == 8) {
    client->format.redMax = 7;
    client->format.greenMax = 7;
    client->format.blueMax = 3;
    client->format.redShift = 0;
    client->format.greenShift = 3;
    client->format.blueShift = 6;
  } else {
    client->format.redMax = (1 << bitsPerSample) - 1;
    client->format.greenMax = (1 << bitsPerSample) - 1;
    client->format.blueMax = (1 << bitsPerSample) - 1;
    if(!client->format.bigEndian) {
      client->format.redShift = 0;
      client->format.greenShift = bitsPerSample;
      client->format.blueShift = bitsPerSample * 2;
    } else {
      if(client->format.bitsPerPixel==8*3) {
	client->format.redShift = bitsPerSample*2;
	client->format.greenShift = bitsPerSample*1;
	client->format.blueShift = 0;
      } else {
	client->format.redShift = bitsPerSample*3;
	client->format.greenShift = bitsPerSample*2;
	client->format.blueShift = bitsPerSample;
      }
    }
  }

  client->bufoutptr=client->buf;
  client->buffered=0;

#ifdef LIBVNCSERVER_HAVE_LIBZ
  client->raw_buffer_size = -1;
  client->decompStreamInited = FALSE;

#ifdef LIBVNCSERVER_HAVE_LIBJPEG
  memset(client->zlibStreamActive,0,sizeof(rfbBool)*4);
#endif
#endif

  client->HandleCursorPos = DummyPoint;
  client->SoftCursorLockArea = DummyRect;
  client->SoftCursorUnlockScreen = Dummy;
  client->GotFrameBufferUpdate = DummyRect;
  client->GotCopyRect = CopyRectangleFromRectangle;
  client->GotFillRect = FillRectangle;
  client->GotBitmap = CopyRectangle;
  client->FinishedFrameBufferUpdate = NULL;
  client->GetPassword = ReadPassword;
  client->MallocFrameBuffer = MallocFrameBuffer;
  client->Bell = Dummy;
  client->CurrentKeyboardLedState = 0;
  client->HandleKeyboardLedState = (HandleKeyboardLedStateProc)DummyPoint;
  client->QoS_DSCP = 0;

  client->authScheme = 0;
  client->subAuthScheme = 0;
  client->GetCredential = GetCredentials;
  client->tlsSession = NULL;
  client->LockWriteToTLS = NULL;
  client->UnlockWriteToTLS = NULL;
  client->sock = RFB_INVALID_SOCKET;
  client->listenSock = RFB_INVALID_SOCKET;
  client->listenAddress = NULL;
  client->listen6Sock = RFB_INVALID_SOCKET;
  client->listen6Address = NULL;
  client->clientAuthSchemes = NULL;

#ifdef LIBVNCSERVER_HAVE_SASL
  client->GetSASLMechanism = NULL;
  client->GetUser = NULL;
  client->saslSecret = NULL;
#endif /* LIBVNCSERVER_HAVE_SASL */

  client->requestedResize = FALSE;
  client->screen.width = 0;
  client->screen.height = 0;

  return client;
}

void rfbClientCleanup(rfbClient* client) {
#ifdef LIBVNCSERVER_HAVE_LIBZ
  int i;

  for ( i = 0; i < 4; i++ ) {
    if (client->zlibStreamActive[i] == TRUE ) {
      if (inflateEnd (&client->zlibStream[i]) != Z_OK &&
	  client->zlibStream[i].msg != NULL)
	rfbClientLog("inflateEnd: %s\n", client->zlibStream[i].msg);
    }
  }

  if ( client->decompStreamInited == TRUE ) {
    if (inflateEnd (&client->decompStream) != Z_OK &&
	client->decompStream.msg != NULL)
      rfbClientLog("inflateEnd: %s\n", client->decompStream.msg );
  }
#endif

  if (client->ultra_buffer)
    free(client->ultra_buffer);

  if (client->raw_buffer)
    free(client->raw_buffer);

  FreeTLS(client);

  while (client->clientData) {
    rfbClientData* next = client->clientData->next;
    free(client->clientData);
    client->clientData = next;
  }

  free(client->vncRec);

  if (client->sock != RFB_INVALID_SOCKET)
    rfbCloseSocket(client->sock);
  if (client->listenSock != RFB_INVALID_SOCKET)
    rfbCloseSocket(client->listenSock);
  free(client->desktopName);
  free(client->serverHost);
  if (client->destHost)
    free(client->destHost);
  if (client->clientAuthSchemes)
    free(client->clientAuthSchemes);

#ifdef LIBVNCSERVER_HAVE_SASL
  if (client->saslSecret)
    free(client->saslSecret);
#endif /* LIBVNCSERVER_HAVE_SASL */

  free(client);
}
