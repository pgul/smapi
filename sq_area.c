/***************************************************************************
 *                                                                         *
 *  Squish Developers Kit Source, Version 2.00                             *
 *  Copyright 1989-1994 by SCI Communications.  All rights reserved.       *
 *                                                                         *
 *  USE OF THIS FILE IS SUBJECT TO THE RESTRICTIONS CONTAINED IN THE       *
 *  SQUISH DEVELOPERS KIT LICENSING AGREEMENT IN SQDEV.PRN.  IF YOU DO NOT *
 *  FIND THE TEXT OF THIS AGREEMENT IN THE AFOREMENTIONED FILE, OR IF YOU  *
 *  DO NOT HAVE THIS FILE, YOU SHOULD IMMEDIATELY CONTACT THE AUTHOR AT    *
 *  ONE OF THE ADDRESSES LISTED BELOW.  IN NO EVENT SHOULD YOU PROCEED TO  *
 *  USE THIS FILE WITHOUT HAVING ACCEPTED THE TERMS OF THE SQUISH          *
 *  DEVELOPERS KIT LICENSING AGREEMENT, OR SUCH OTHER AGREEMENT AS YOU ARE *
 *  ABLE TO REACH WITH THE AUTHOR.                                         *
 *                                                                         *
 *  You can contact the author at one of the address listed below:         *
 *                                                                         *
 *  Scott Dudley       FidoNet     1:249/106                               *
 *  777 Downing St.    Internet    sjd@f106.n249.z1.fidonet.org            *
 *  Kingston, Ont.     CompuServe  >INTERNET:sjd@f106.n249.z1.fidonet.org  *
 *  Canada  K7M 5N3    BBS         1-613-634-3058, V.32bis                 *
 *                                                                         *
 ***************************************************************************/
/*
#pragma off(unreferenced)
static char rcs_id[]="$Id$";
#pragma on(unreferenced)
*/
#define MSGAPI_HANDLERS
#define MSGAPI_NO_OLD_TYPES

#if !defined(UNIX) && !defined(SASC)
#include <io.h>
#endif

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#if !defined(UNIX) && !defined(SASC)
#include <share.h>
#endif

#include <string.h>
#include <stdlib.h>

#ifdef UNIX
#include <unistd.h>
#endif

#if defined(UNIX) && !defined(__BEOS__)
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#endif

#include "prog.h"
#include "alc.h"
#include "old_msg.h"
#include "msgapi.h"
#include "api_sq.h"
#include "api_sqp.h"
#include "apidebug.h"
#include "unused.h"

/* Linked list of open Squish areas */

static HAREA haOpen=NULL;

#ifdef __IBMC__
static short lock_sem = 0;
#elif defined(__BEOS__)
#include <OS.h>
sem_id lock_sem;
#elif defined(UNIX)
int lock_sem;
#endif

static char dot_sqd[]=".sqd";
static char dot_sqi[]=".sqi";
static char dot_sql[]=".sql";

void _SquishInitSem()
{
#ifdef __BEOS__
  lock_sem = create_sem(0,0);
  release_sem(lock_sem);
#elif defined(UNIX)
  lock_sem = semget(IPC_PRIVATE, 1, 0);
#endif
}

void _SquishFreeSem()
{
#ifdef __BEOS__
  delete_sem(lock_sem);
#elif defined(UNIX)
  struct sembuf sops;
  union semun fl;

  fl.val = 0;

  semctl(lock_sem, 0, IPC_RMID, fl);
  
  sops.sem_num = 0;
  sops.sem_op  = 1;
  sops.sem_flg = 0;

  semop(lock_sem, &sops, 1);
#endif
}

short _SquishThreadLock(void)
{
#ifdef __IBMC__
  while (__sxchg(&lock_sem,1) )
    tdelay(10);
#elif defined(__BEOS__)
  if (acquire_sem(lock_sem) != B_NO_ERROR)
    tdelay(10);
#elif defined(UNIX)
  struct sembuf sops;
  
  sops.sem_num = 0;
  sops.sem_op  = -1;
  sops.sem_flg = 0;

  while (semop(lock_sem, &sops, 1))
    usleep(10);
#endif
  return 1;
}

short _SquishThreadUnlock(void)
{
#ifdef __IBMC__
  __sxchg(&lock_sem,0);
#elif defined(__BEOS__)
  release_sem(lock_sem);
#elif defined(UNIX)
  struct sembuf sops;
  
  sops.sem_num = 0;
  sops.sem_op  = 1;
  sops.sem_flg = 0;

  semop(lock_sem, &sops, 1);
#endif
  return 1;
}

int SquishDeleteBase(char *name)
{
    byte temp[PATHLEN];
    int rc = 1;
    
    sprintf((char *) temp, (char *) dot_sqd, name);
    if (unlink(temp)) rc = 0;
    sprintf((char *) temp, (char *) dot_sqi, name);
    if (unlink(temp)) rc = 0;
    sprintf((char *) temp, (char *) dot_sql, name);
    unlink(temp);
    return rc;
}   

/* Exitlist routine to make sure that all areas are closed */

unsigned _SquishCloseOpenAreas(void)
{
  HAREA ha, haNext;

  /* If nothing to close, just get out. */

  if (!haOpen)
    return TRUE;

  for (ha=haOpen; ha; ha=haNext)
  {
    haNext=Sqd->haNext;

    SquishCloseArea(ha);
  }

  haOpen=NULL;

  return TRUE;
}

/* List of function pointers to use in Squish areas */

static struct _apifuncs sq_funcs=
{
  SquishCloseArea,
  SquishOpenMsg,
  SquishCloseMsg,
  SquishReadMsg,
  SquishWriteMsg,
  SquishKillMsg,
  SquishLock,
  SquishUnlock,
  SquishSetCurPos,
  SquishGetCurPos,
  SquishMsgnToUid,
  SquishUidToMsgn,
  SquishGetHighWater,
  SquishSetHighWater,
  SquishGetTextLen,
  SquishGetCtrlLen,
  SquishGetNextUid,
  SquishGetHash
};


/* Open the .SQD and .SQI files for an existing base */

static unsigned near _SquishOpenBaseFiles(HAREA ha, byte  *szName, int mode)
{
  char szFile[PATHLEN];

  (void)strcpy(szFile, szName);
  (void)strcat(szFile, dot_sqd);

  if ((Sqd->sfd=sopen(szFile, mode | O_RDWR | O_BINARY, SH_DENYNO,
                      S_IREAD | S_IWRITE))==-1)
  {
    msgapierr=MERR_NOENT;
    return FALSE;
  }
  (void)strcpy(szFile, szName);
  (void)strcat(szFile, dot_sqi);

  if ((Sqd->ifd=sopen(szFile, mode | O_RDWR | O_BINARY, SH_DENYNO,
                      S_IREAD | S_IWRITE))==-1)
  {
    (void)close(Sqd->sfd);
    msgapierr=MERR_NOENT;
    return FALSE;
  }

  return TRUE;
}



/* Delete the .SQD and .SQI files for an area */

static unsigned near _SquishUnlinkBaseFiles(byte  *szName)
{
  char szFile[PATHLEN];
  unsigned rc=TRUE;

  (void)strcpy(szFile, szName);
  (void)strcat(szFile, dot_sqd);

  if (unlink(szFile) != 0)
    rc=FALSE;

  (void)strcpy(szFile, szName);
  (void)strcat(szFile, dot_sqi);

  if (unlink(szFile) != 0)
    rc=FALSE;

  return rc;
}




/* Close the data files for this message base */

static void near _SquishCloseBaseFiles(HAREA ha)
{
  (void)close(Sqd->sfd);
  (void)close(Sqd->ifd);

  Sqd->sfd=-1;
  Sqd->ifd=-1;
}


/* Ensure that the SQBASE header is valid */

static unsigned near _SquishValidateBaseHeader(SQBASE *psqb)
{
  if (psqb->num_msg > psqb->high_msg ||
      psqb->num_msg > psqb->uid+1 ||
      psqb->high_msg > psqb->uid+1 ||
      psqb->num_msg > 1000000L ||
      psqb->num_msg != psqb->high_msg ||
      psqb->len < SQBASE_SIZE ||
      psqb->len >= 1024 ||
      psqb->begin_frame > psqb->end_frame ||
      psqb->last_frame > psqb->end_frame ||
      psqb->free_frame > psqb->end_frame ||
      psqb->last_free_frame > psqb->end_frame ||
      psqb->end_frame==0)
  {
    msgapierr=MERR_BADF;
    return FALSE;
  }

  return TRUE;
}


/* Copy information from the psqb disk header to our in-memory struct */

unsigned _SquishCopyBaseToData(HAREA ha, SQBASE *psqb)
{
  Sqd->cbSqbase=psqb->len;
  Sqd->cbSqhdr=psqb->sz_sqhdr;
  Sqd->wSkipMsg=(word)psqb->skip_msg;
  Sqd->dwMaxMsg=psqb->max_msg;
  Sqd->wMaxDays=psqb->keep_days;
  Sqd->dwHighWater=psqb->high_water;
  Sqd->uidNext=psqb->uid;
  Sqd->foFirst=psqb->begin_frame;
  Sqd->foLast=psqb->last_frame;
  Sqd->foFree=psqb->free_frame;
  Sqd->foLastFree=psqb->last_free_frame;
  Sqd->foEnd=psqb->end_frame;
  Sqd->sqbDelta=*psqb;

  ha->num_msg=psqb->num_msg;
  ha->high_msg=psqb->num_msg;
  ha->high_water=psqb->high_water;

  return TRUE;
}


/* Copy data from the in-memory struct into the disk header */

unsigned _SquishCopyDataToBase(HAREA ha, SQBASE *psqb)
{
  (void)memset(psqb, 0, sizeof(SQBASE));

  psqb->len=Sqd->cbSqbase;
  psqb->sz_sqhdr=Sqd->cbSqhdr;
  psqb->skip_msg=Sqd->wSkipMsg;
  psqb->max_msg=Sqd->dwMaxMsg;
  psqb->keep_days=Sqd->wMaxDays;
  psqb->high_water=Sqd->dwHighWater;
  psqb->uid=Sqd->uidNext;
  psqb->begin_frame=Sqd->foFirst;
  psqb->last_frame=Sqd->foLast;
  psqb->free_frame=Sqd->foFree;
  psqb->last_free_frame=Sqd->foLastFree;
  psqb->end_frame=Sqd->foEnd;

  psqb->num_msg=ha->num_msg;
  psqb->high_msg=ha->high_msg;
  psqb->high_water=ha->high_water;

  return TRUE;
}


/* Set the starting values for this message base */

static unsigned near _SquishSetBaseDefaults(HAREA ha)
{
  /* Set up our current position in the linked list */

  Sqd->foNext=Sqd->foFirst;
  Sqd->foCur=NULL_FRAME;
  Sqd->foPrev=NULL_FRAME;
  ha->cur_msg=0;
  ha->sz_xmsg=XMSG_SIZE;
  return TRUE;
}

/* Fill out the initial values in a Squish base header */

static unsigned near _SquishFillBaseHeader(SQBASE *psqb, byte  *szName)
{
  psqb->len=SQBASE_SIZE;
  psqb->rsvd1=0;
  psqb->num_msg=0L;
  psqb->high_msg=0L;
  psqb->skip_msg=0L;
  psqb->high_water=0L;
  psqb->uid=1L;
  (void)strcpy(psqb->base, szName);
  psqb->begin_frame=NULL_FRAME;
  psqb->last_frame=NULL_FRAME;
  psqb->free_frame=NULL_FRAME;
  psqb->last_free_frame=NULL_FRAME;
  psqb->end_frame=SQBASE_SIZE;
  psqb->max_msg=0L;
  psqb->keep_days=0;
  psqb->sz_sqhdr=SQHDR_SIZE;
  (void)memset(psqb->rsvd2, 0, sizeof psqb->rsvd2);

  return TRUE;
}


/* Create a new Squish message area */

static unsigned near _SquishCreateNewBase(HAREA ha, byte  *szName)
{
  SQBASE sqb;           /* Header from Squish base */

  /* Try to open the files */

  if (!_SquishOpenBaseFiles(ha, szName, O_CREAT | O_TRUNC))
    return FALSE;

  if (!_SquishFillBaseHeader(&sqb, szName) ||
      !_SquishWriteBaseHeader(ha, &sqb) ||
      !_SquishCopyBaseToData(ha, &sqb) ||
      !_SquishSetBaseDefaults(ha))
  {
    /* The open failed, so delete the partially-created Squishbase */
    _SquishCloseBaseFiles(ha);
    (void)_SquishUnlinkBaseFiles(szName);

    return FALSE;
  }

  return TRUE;
}


/* Open an existing Squish base and fill out 'ha' appropriately */

static unsigned near _SquishOpenExistingBase(HAREA ha, byte  *szName)
{
  SQBASE sqb;           /* Header from Squish base */

  /* Try to open the files */

  if (!_SquishOpenBaseFiles(ha, szName, 0))
    return FALSE;

  if (!_SquishReadBaseHeader(ha, &sqb) ||
      !_SquishValidateBaseHeader(&sqb) ||
      !_SquishCopyBaseToData(ha, &sqb) ||
      !_SquishSetBaseDefaults(ha))
  {
    _SquishCloseBaseFiles(ha);
    return FALSE;
  }

  return TRUE;
}



/* Allocate a new area handle */

static HAREA NewHarea(word wType)
{
  HAREA ha;

  /* Try to allocate memory for the area handle */

  if ((ha=(HAREA)palloc(sizeof(*ha)))==NULL)
    return NULL;

  (void)memset(ha, 0, sizeof *ha);

  ha->id=MSGAPI_ID;
  ha->len=sizeof(struct _msgapi);
  ha->type=wType & ~MSGTYPE_ECHO;
  ha->isecho=(byte)!!(wType & MSGTYPE_ECHO);

  return ha;
}

/* Open a Squish base */

HAREA MSGAPI SquishOpenArea(byte  *szName, word wMode, word wType)
{
  HAREA ha;                       /* Area handle for this area */
  unsigned fOpened;               /* Has this area been opened? */

  /* Make sure that we have a valid base name */

  if (!szName)
  {
    msgapierr=MERR_BADA;
    return NULL;
  }

  /* Allocate memory for the Squish handle */

  if ((ha=NewHarea(wType))==NULL)
    return NULL;

  /* Allocate memory for the Squish-specific part of the handle */

  if ((ha->apidata=(void *)palloc(sizeof(struct _sqdata)))==NULL)
  {
    pfree(ha);
    return NULL;
  }

  memset(ha->apidata, 0, sizeof(struct _sqdata));


  /* Allocate memory to hold the function pointers */

  if ((ha->api=(struct _apifuncs *)palloc(sizeof(struct _apifuncs)))==NULL)
  {
    pfree(ha->apidata);
    pfree(ha);
    return NULL;
  }

  /* Fill out the function pointers for this area */

  *ha->api = sq_funcs;
  
  /* Open the index interface for this area */

  if ((Sqd->hix=_SquishOpenIndex(ha))==NULL)
    return NULL;

  fOpened=FALSE;

  /* If we want to open an existing area, try it here */

  msgapierr=0;

  if (wMode==MSGAREA_NORMAL || wMode==MSGAREA_CRIFNEC)
    fOpened=_SquishOpenExistingBase(ha, szName);
  else
    msgapierr=MERR_NOENT;

  /* If we want to create a new area, try that now */

  if (msgapierr==MERR_NOENT &&
      (wMode==MSGAREA_CREATE ||
       (wMode==MSGAREA_CRIFNEC && !fOpened)))
  {
    fOpened=_SquishCreateNewBase(ha, szName);
  }

  /* If the open succeeded */

  _SquishThreadLock();

  if (fOpened)
  {
    /* Add us to the linked list of open areas */

    Sqd->haNext=haOpen;
    haOpen=ha;
  }
  else
  {
    pfree(ha->apidata);
    pfree(ha->api);
    pfree(ha);
    ha=NULL;
  }

  _SquishThreadUnlock();

#ifdef __BEOS__
  ha->sem = create_sem(0, 0);
  release_sem(ha->sem);
#elif defined(UNIX)
  ha->sem = semget(IPC_PRIVATE, 1, 0);
  _SquishBaseThreadUnlock(ha);
#endif

  /* Return the handle to this area */
  return ha;
}

/* Close any messages in this area which may be open */

static unsigned near _SquishCloseAreaMsgs(HAREA ha)
{
  HMSG hm, hmNext;

  /* Close any open messages, if necessary */

  for (hm=Sqd->hmsgOpen; hm; hm=hmNext)
  {
    hmNext=hm->hmsgNext;

    if (SquishCloseMsg(hm)==-1)
    {
      msgapierr=MERR_EOPEN;
      return FALSE;
    }
  }

  return TRUE;
}


static unsigned near _SquishRemoveAreaList(HAREA haThis)
{
  HAREA ha, haNext;

  if (!haOpen)
  {
    msgapierr=MERR_BADA;
    return FALSE;
  }

  /* If we were at the head of the list, adjust the main pointer only */

  _SquishThreadLock();

  if (haOpen==haThis)
  {
    ha=haThis;
    haOpen=Sqd->haNext;
    _SquishThreadUnlock();
    return TRUE;
  }

  /* Try to find us in the middle of the list */

  for (ha=haOpen; ha; ha=haNext)
  {
    haNext=Sqd->haNext;

    if (haNext==haThis)
    {
      Sqd->haNext=((SQDATA *)(haThis->apidata))->haNext;
      _SquishThreadUnlock();
      return TRUE;
    }
  }

  _SquishThreadUnlock();

  msgapierr=MERR_BADA;
  return FALSE;
}


/* Close an open message area */

sword EXPENTRY SquishCloseArea(HAREA ha)
{
  if (MsgInvalidHarea(ha))
    return -1;

  /* Close any open messages */

  if (!_SquishCloseAreaMsgs(ha))
    return -1;

  /* Unlock the area, if necessary */

  if (Sqd->fHaveExclusive)
  {
    Sqd->fHaveExclusive=1;
    (void)_SquishExclusiveEnd(ha);
  }


  /* Unlock the area as well */

  if (Sqd->fLockFunc)
  {
    if (Sqd->fLocked)
      Sqd->fLocked=1;

    Sqd->fLockFunc=1;

    SquishUnlock(ha);
  }

  (void)_SquishCloseIndex(Sqd->hix);

  /* Close off the Squish data files */

  _SquishCloseBaseFiles(ha);

  /* Remove ourselves from the list of open areas */

  (void)_SquishRemoveAreaList(ha);

  /* Blank out the ID, then free all of the memory associated with this     *
   * area handle.                                                           */

  ha->id=0;

#ifdef __BEOS__
  delete_sem(ha->sem);
#elif defined (UNIX)
  {
    union semun fl;
    fl.val = 0;
    semctl(ha->sem, 0, IPC_RMID, fl);
  }
#endif

  pfree(ha->api);
  pfree(ha->apidata);
  pfree(ha);

  return 0;
}


/* This function ensures that the specified Squish base name exists */

sword MSGAPI SquishValidate(byte  *szName)
{
  char szFile[PATHLEN];

  (void)strcpy(szFile, szName);
  (void)strcat(szFile, dot_sqd);

  if (!fexist(szFile))
    return FALSE;

  (void)strcpy(szFile, szName);
  (void)strcat(szFile, dot_sqi);

  return fexist(szFile);
}

