/*******************************************************************************


   Copyright (C) 2011-2014 SequoiaDB Ltd.

   This program is free software: you can redistribute it and/or modify
   it under the term of the GNU Affero General Public License, version 3,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warrenty of
   MARCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program. If not, see <http://www.gnu.org/license/>.

   Source File Name = omManager.cpp

   Dependencies: N/A

   Restrictions: N/A

   Change Activity:
   defect Date        Who Description
   ====== =========== === ==============================================
          04/15/2014  XJH Initial Draft

   Last Changed =

*******************************************************************************/

#include "omManager.hpp"
#include "../bson/lib/md5.hpp"
#include "authCB.hpp"
#include "pmdEDU.hpp"
#include "pmd.hpp"
#include "../bson/bsonobj.h"
#include "../util/fromjson.hpp"
#include "catCommon.hpp"
#include "../bson/lib/md5.hpp"
#include "ossProc.hpp"
#include "rtn.hpp"
#include "omManagerJob.hpp"
#include "../omsvc/omGetFileCommand.hpp"

using namespace bson ;

namespace engine
{

   #define OM_WAIT_CB_ATTACH_TIMEOUT               ( 300 * OSS_ONE_SEC )

   /*
      Message Map
   */
   BEGIN_OBJ_MSG_MAP( _omManager, _pmdObjBase )
      ON_MSG( MSG_BS_QUERY_REQ, _onAgentQueryTaskReq )
   END_OBJ_MSG_MAP()

   /*
      implement om manager
   */
   _omManager::_omManager()
   :_fixBufSize( SDB_PAGE_SIZE ),
    _rsManager(),
    _msgHandler( &_rsManager ),
    _netAgent( &_msgHandler )
   {
      _maxRestBodySize     = OM_REST_MAX_BODY_SIZE ;
      _restTimeout         = REST_TIMEOUT ;
      _sequence            = 1 ;
      _checkSessionTimer   = NET_INVALID_TIMER_ID ;

      _hwRouteID.value     = MSG_INVALID_ROUTEID ;
      _hwRouteID.columns.groupID = 2 ;
      _hwRouteID.columns.nodeID  = 0 ;
      _hwRouteID.columns.serviceID = MSG_ROUTE_LOCAL_SERVICE ;

      _pKrcb               = NULL ;
      _pDmsCB              = NULL ;
      _hostVersion         = SDB_OSS_NEW omHostVersion() ;
      _taskManager         = SDB_OSS_NEW omTaskManager( this ) ;
   }

   _omManager::~_omManager()
   {
      SDB_ASSERT( _vecFixBuf.size() == 0, "Fix buff catch must be empty" ) ;
      if ( NULL != _hostVersion )
      {
         SDB_OSS_DEL _hostVersion ;
         _hostVersion = NULL ;
      }

      if ( NULL != _taskManager )
      {
         SDB_OSS_DEL _taskManager ;
         _taskManager = NULL ;
      }
   }

   INT32 _omManager::init ()
   {
      INT32 rc           = SDB_OK ;

      _pKrcb  = pmdGetKRCB() ;
      _pDmsCB = _pKrcb->getDMSCB() ;
      _pRtnCB = _pKrcb->getRTNCB() ;

      _pmdOptionsMgr *pOptMgr = _pKrcb->getOptionCB() ;

      _wwwRootPath = pmdGetOptionCB()->getWWWPath() ;

      rc = _rsManager.init( getRouteAgent() ) ;
      PD_RC_CHECK( rc, PDERROR, "Failed to init remote session manager, rc: %d",
                   rc ) ;

      rc = _initOmTables();
      PD_RC_CHECK ( rc, PDERROR, "Failed to initial the om tables rc = %d", 
                    rc ) ;

      rc = _restoreTask() ;
      PD_RC_CHECK ( rc, PDERROR, "Failed to restore task:rc=%d", 
                    rc ) ;

      rc = _createJobs() ;
      PD_RC_CHECK ( rc, PDERROR, "Failed to create jobs:rc=%d", 
                    rc ) ;

      rc = refreshVersions() ;
      PD_RC_CHECK ( rc, PDERROR, "Failed to update cluster version:rc=%d", 
                    rc ) ;

      _readAgentPort() ;

      rc = _restAdptor.init( _fixBufSize, _maxRestBodySize, _restTimeout ) ;
      PD_RC_CHECK( rc, PDERROR, "Failed to init rest adptor, rc: %d", rc ) ;

      _myNodeID.value             = MSG_INVALID_ROUTEID ;
      _myNodeID.columns.serviceID = MSG_ROUTE_LOCAL_SERVICE ;
      _netAgent.updateRoute( _myNodeID, _pKrcb->getHostName(), 
                             pOptMgr->getOMService() ) ;
      rc = _netAgent.listen( _myNodeID ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG ( PDERROR, "Create listen failed:host=%s,port=%s", 
                  _pKrcb->getHostName(), pOptMgr->getOMService() ) ;
         goto error ;
      }

      PD_LOG ( PDEVENT, "Create listen success:host=%s,port=%s",
               _pKrcb->getHostName(), pOptMgr->getOMService() ) ;

   done:
      return rc;
   error:
      goto done;

   }

   INT32 _omManager::_createJobs()
   {
      INT32 rc                = SDB_OK ;
      BOOLEAN returnResult    = FALSE ;
      _rtnBaseJob *pJob       = NULL ;
      EDUID jobID             = PMD_INVALID_EDUID ;
      pJob = SDB_OSS_NEW omHostNotifierJob( this, _hostVersion ) ;
      if ( !pJob )
      {
         rc = SDB_OOM ;
         PD_LOG ( PDERROR, "failed to create omHostNotifierJob:rc=%d", rc ) ;
         goto error ;
      }
      rc = rtnGetJobMgr()->startJob( pJob, RTN_JOB_MUTEX_NONE, &jobID,
                                     returnResult ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "create omHostNotifierJob failed:rc=%d", rc ) ;
         goto error ;
      }

      pJob = SDB_OSS_NEW omTaskJob( this, this->getTaskManager() ) ;
      if ( !pJob )
      {
         rc = SDB_OOM ;
         PD_LOG ( PDERROR, "failed to create omTaskJob:rc=%d", rc ) ;
         goto error ;
      }
      rc = rtnGetJobMgr()->startJob( pJob, RTN_JOB_MUTEX_NONE, &jobID,
                                     returnResult ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "create omTaskJob failed:rc=%d", rc ) ;
         goto error ;
      }
   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omManager::refreshVersions()
   {
      INT32 rc = SDB_OK ;
      BSONObj selector ;
      BSONObj matcher ;
      BSONObj order ;
      BSONObj hint ;
      BSONObjBuilder builder ;
      SINT64 contextID = -1 ;

      BSONObjBuilder resultBuilder ;
      BSONObj result ;
      pmdKRCB *pKrcb     = pmdGetKRCB() ;
      _SDB_DMSCB *pDMSCB = pKrcb->getDMSCB() ;
      _SDB_RTNCB *pRTNCB = pKrcb->getRTNCB() ;
      _pmdEDUCB *pEDUCB  = pmdGetThreadEDUCB() ;

      selector = BSON( OM_CLUSTER_FIELD_NAME << "" ) ;
      rc = rtnQuery( OM_CS_DEPLOY_CL_CLUSTER, selector, matcher, order, hint, 0, 
                     pEDUCB, 0, -1, pDMSCB, pRTNCB, contextID );
      if ( rc )
      {
         PD_LOG( PDERROR, "fail to query table:%s,rc=%d", 
                 OM_CS_DEPLOY_CL_CLUSTER, rc ) ;
         goto error ;
      }

      while ( TRUE )
      {
         rtnContextBuf buffObj ;
         rc = rtnGetMore( contextID, 1, buffObj, pEDUCB, pRTNCB ) ;
         if ( rc )
         {
            if ( SDB_DMS_EOC == rc )
            {
               rc = SDB_OK ;
               break ;
            }

            contextID = -1 ;
            PD_LOG( PDERROR, "failed to get record from table:%s,rc=%d", 
                    OM_CS_DEPLOY_CL_CLUSTER, rc ) ;
            goto error ;
         }

         BSONObj record( buffObj.data() ) ;
         string clusterName = record.getStringField( OM_CLUSTER_FIELD_NAME ) ;
         _hostVersion->incVersion( clusterName ) ;
      }
   done:
      return rc ;
   error:
      if ( -1 != contextID )
      {
         pRTNCB->contextDelete( contextID, pEDUCB ) ;
      }
      goto done ;
   }

   void _omManager::updateClusterVersion( string cluster )
   {
      _hostVersion->incVersion( cluster ) ;
   }

   void _omManager::removeClusterVersion( string cluster )
   {
      _hostVersion->removeVersion( cluster ) ;
   }

   omTaskManager *_omManager::getTaskManager()
   {
      return _taskManager ;
   }

   INT32 _omManager::_initOmTables() 
   {
      _pmdEDUCB *cb       = NULL ;
      INT32 rc            = SDB_OK ;
      BSONObjBuilder bsonBuilder ;
      SDB_AUTHCB *pAuthCB = NULL ;

      cb = pmdGetThreadEDUCB() ;

      rc = _createCollection ( OM_CS_DEPLOY_CL_CLUSTER, cb ) ;
      if ( rc )
      {
         goto error ;
      }
      rc = _createCollectionIndex ( OM_CS_DEPLOY_CL_CLUSTER,
                                    OM_CS_DEPLOY_CL_CLUSTERIDX1, cb ) ;
      if ( rc )
      {
         goto error ;
      }

      rc = _createCollection ( OM_CS_DEPLOY_CL_HOST, cb ) ;
      if ( rc )
      {
         goto error ;
      }
      rc = _createCollectionIndex ( OM_CS_DEPLOY_CL_HOST,
                                    OM_CS_DEPLOY_CL_HOSTIDX1, cb ) ;
      if ( rc )
      {
         goto error ;
      }
      rc = _createCollectionIndex ( OM_CS_DEPLOY_CL_HOST,
                                    OM_CS_DEPLOY_CL_HOSTIDX2, cb ) ;
      if ( rc )
      {
         goto error ;
      }

      rc = _createCollection ( OM_CS_DEPLOY_CL_BUSINESS, cb ) ;
      if ( rc )
      {
         goto error ;
      }
      rc = _createCollectionIndex ( OM_CS_DEPLOY_CL_BUSINESS,
                             OM_CS_DEPLOY_CL_BUSINESSIDX1, cb ) ;
      if ( rc )
      {
         goto error ;
      }

      rc = _createCollection ( OM_CS_DEPLOY_CL_CONFIGURE, cb ) ;
      if ( rc )
      {
         goto error ;
      }

      rc = _createCollection ( OM_CS_DEPLOY_CL_TASKINFO, cb ) ;
      if ( rc )
      {
         goto error ;
      }
      rc = _createCollectionIndex ( OM_CS_DEPLOY_CL_TASKINFO,
                             OM_CS_DEPLOY_CL_TASKINFOIDX1, cb ) ;
      if ( rc )
      {
         goto error ;
      }

      pAuthCB = pmdGetKRCB()->getAuthCB() ;
      pAuthCB->checkNeedAuth( cb, TRUE ) ;
      if ( !pAuthCB->needAuthenticate() )
      {
         md5::md5digest digest ;
         BSONObj obj ;
         bsonBuilder.append( SDB_AUTH_USER, OM_DEFAULT_LOGIN_USER ) ;
         md5::md5( ( const void * )OM_DEFAULT_LOGIN_PASSWD, 
                   ossStrlen( OM_DEFAULT_LOGIN_PASSWD ), digest) ;
         bsonBuilder.append( SDB_AUTH_PASSWD, md5::digestToString( digest ) ) ;
         obj = bsonBuilder.obj() ;
         rc = pAuthCB->createUsr( obj, cb ) ;
         if ( SDB_IXM_DUP_KEY == rc )
         {
            rc = SDB_OK ;
         }
         PD_RC_CHECK ( rc, PDERROR, "Failed to create default user:rc = %d",
                       rc ) ;
      }
      pAuthCB->checkNeedAuth( cb, TRUE ) ;

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omManager::_createCollectionIndex ( const CHAR *pCollection,
                                              const CHAR *pIndex,
                                              pmdEDUCB *cb )
   {
      INT32 rc = SDB_OK ;
      BSONObj indexDef ;

      rc = fromjson ( pIndex, indexDef ) ;
      PD_RC_CHECK ( rc, PDERROR, "Failed to build index object, rc = %d",
                    rc ) ;

      rc = catTestAndCreateIndex( pCollection, indexDef, cb, _pDmsCB,
                                  NULL, TRUE ) ;
      if ( rc )
      {
         goto error ;
      }

   done :
      return rc ;
   error :
      goto done ;
   }

   INT32 _omManager::_createCollection ( const CHAR *pCollection, pmdEDUCB *cb )
   {
      return catTestAndCreateCL( pCollection, cb, _pDmsCB, NULL, TRUE ) ;
   }

   INT32 _omManager::active ()
   {
      INT32 rc = SDB_OK ;
      pmdEDUMgr *pEDUMgr = pmdGetKRCB()->getEDUMgr() ;
      EDUID eduID = PMD_INVALID_EDUID ;

      pmdSetPrimary( TRUE ) ;

      rc = pEDUMgr->startEDU( EDU_TYPE_OMMGR, (_pmdObjBase*)this, &eduID ) ;
      PD_RC_CHECK( rc, PDERROR, "Failed to start OM Manager edu, rc: %d", rc ) ;
      pEDUMgr->regSystemEDU( EDU_TYPE_OMMGR, eduID ) ;
      rc = _attachEvent.wait( OM_WAIT_CB_ATTACH_TIMEOUT ) ;
      PD_RC_CHECK( rc, PDERROR, "Wait OM Manager edu attach failed, rc: %d",
                   rc ) ;

      _checkSessionTimer = setTimer( 60 * OSS_ONE_SEC ) ;
      if ( NET_INVALID_TIMER_ID == _checkSessionTimer )
      {
         rc = SDB_SYS ;
         PD_LOG( PDERROR, "Failed to set timer" ) ;
         goto error ;
      }

      rc = pEDUMgr->startEDU( EDU_TYPE_OMNET, (netRouteAgent*)&_netAgent,
                              &eduID ) ;
      PD_RC_CHECK( rc, PDERROR, "Failed to start om net, rc: %d", rc ) ;
      pEDUMgr->regSystemEDU( EDU_TYPE_OMNET, eduID ) ;

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omManager::deactive ()
   {
      _netAgent.closeListen() ;
      _netAgent.stop() ;

      if ( NET_INVALID_TIMER_ID != _checkSessionTimer )
      {
         killTimer( _checkSessionTimer ) ;
         _checkSessionTimer = NET_INVALID_TIMER_ID ;
      }

      return SDB_OK ;
   }

   INT32 _omManager::fini ()
   {
      _rsManager.fini() ;

      _omLatch.get() ;
      for ( UINT32 i = 0 ; i < _vecFixBuf.size() ; ++i )
      {
         SDB_OSS_FREE( OM_FIX_BUFF_TO_PTR( _vecFixBuf[i] ) ) ;
      }
      _vecFixBuf.clear() ;
      _omLatch.release() ;

      restSessionInfo *pSessionInfo = NULL ;
      map<string, restSessionInfo*>::iterator it = _mapSessions.begin() ;
      while( it != _mapSessions.end() )
      {
         pSessionInfo = it->second ;
         pSessionInfo->releaseMem() ;
         SDB_OSS_DEL pSessionInfo ;
         ++it ;
      }
      _mapSessions.clear() ;
      _mapUser2Sessions.clear() ;

      _mapID2Host.clear() ;
      _mapHost2ID.clear() ;

      return SDB_OK ;
   }

   void _omManager::attachCB( _pmdEDUCB *cb )
   {
      _rsManager.registerEDU( cb ) ;
      _msgHandler.attach( cb ) ;
      _timerHandler.attach( cb ) ;
      _attachEvent.signalAll() ;
   }

   void _omManager::detachCB( _pmdEDUCB *cb )
   {
      _msgHandler.detach() ;
      _timerHandler.detach() ;
      _rsManager.unregEUD( cb ) ;
   }

   UINT32 _omManager::setTimer( UINT32 milliSec )
   {
      UINT32 timeID = NET_INVALID_TIMER_ID ;
      _netAgent.addTimer( milliSec, &_timerHandler, timeID ) ;
      return timeID ;
   }

   void _omManager::killTimer( UINT32 timerID )
   {
      _netAgent.removeTimer( timerID ) ;
   }

   void _omManager::onTimer( UINT64 timerID, UINT32 interval )
   {
      if ( timerID == _checkSessionTimer )
      {
         _checkSession( interval ) ;
      }
   }

   INT32 _omManager::authenticate( BSONObj &obj, pmdEDUCB *cb )
   {
      INT32 rc = SDB_OK ;
      SDB_AUTHCB *pAuthCB = pmdGetKRCB()->getAuthCB() ;

      if ( !pAuthCB || !pAuthCB->needAuthenticate() )
      {
         goto done ;
      }

      rc = pAuthCB->authenticate( obj, cb ) ;
      if ( rc )
      {
         goto error ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omManager::authUpdatePasswd( string user, string oldPasswd,
                                       string newPasswd, pmdEDUCB *cb )
   {
      INT32 rc            = SDB_OK ;
      SDB_AUTHCB *pAuthCB = pmdGetKRCB()->getAuthCB() ;
      if ( NULL == pAuthCB )
      {
         rc = SDB_INVALIDARG ;
         goto error ;
      }

      rc = pAuthCB->updatePasswd( user, oldPasswd, newPasswd, cb ) ;

   done:
      return rc ;
   error:
      goto done ;
   }

   CHAR* _omManager::allocFixBuf()
   {
      CHAR *pBuff = NULL ;

      _omLatch.get() ;
      if ( _vecFixBuf.size() > 0 )
      {
         pBuff = _vecFixBuf.back() ;
         _vecFixBuf.pop_back() ;
      }
      _omLatch.release() ;

      if ( pBuff )
      {
         goto done ;
      }

      pBuff = ( CHAR* )SDB_OSS_MALLOC( OM_FIX_PTR_SIZE( _fixBufSize ) ) ;
      if ( !pBuff )
      {
         PD_LOG( PDERROR, "Alloc fix buff failed, size: %d",
                 OM_FIX_PTR_SIZE( _fixBufSize ) ) ;
         goto error ;
      }
      OM_FIX_PTR_HEADER( pBuff ) = _fixBufSize ;
      pBuff = OM_FIX_PTR_TO_BUFF( pBuff ) ;

   done:
      return pBuff ;
   error:
      goto done ;
   }

   void _omManager::releaseFixBuf( CHAR * pBuff )
   {
      SDB_ASSERT( pBuff, "Buff can't be NULL" ) ;
      SDB_ASSERT( OM_FIX_BUFF_HEADER( pBuff ) == _fixBufSize,
                  "Buff is not alloc by fix buff" ) ;

      _omLatch.get() ;
      if ( _vecFixBuf.size() < OM_FIX_BUFF_CATCH_NUMBER )
      {
         _vecFixBuf.push_back( pBuff ) ;
         pBuff = NULL ;
      }
      _omLatch.release() ;

      if ( pBuff )
      {
         SDB_OSS_FREE( OM_FIX_BUFF_TO_PTR( pBuff ) ) ;
      }
   }

   restSessionInfo* _omManager::attachSessionInfo( const string &id )
   {
      restSessionInfo *pSessionInfo = NULL ;

      _omLatch.get_shared() ;
      map<string, restSessionInfo*>::iterator it = _mapSessions.find( id ) ;
      if ( it != _mapSessions.end() )
      {
         pSessionInfo = it->second ;
         if ( pSessionInfo->isValid() )
         {
            pSessionInfo->_inNum.inc() ;
         }
         else
         {
            pSessionInfo = NULL ;
         }
      }
      _omLatch.release_shared() ;

      if ( pSessionInfo )
      {
         pSessionInfo->lock() ;
      }

      return pSessionInfo ;
   }

   void _omManager::detachSessionInfo( restSessionInfo * pSessionInfo )
   {
      SDB_ASSERT( pSessionInfo, "Session can't be NULL" ) ;

      if ( pSessionInfo->isLock() )
      {
         pSessionInfo->unlock() ;
         pSessionInfo->_inNum.dec() ;
      }
   }

   void _omManager::_invalidSessionInfo( restSessionInfo *pSessionInfo )
   {
      SDB_ASSERT( pSessionInfo, "Session can't be NULL" ) ;
      pSessionInfo->invalidate() ;
   }

   void _omManager::_checkSession( UINT32 interval )
   {
      map<string, restSessionInfo*>::iterator it  ;
      restSessionInfo *pInfo = NULL ;

      _omLatch.get() ;
      it = _mapSessions.begin() ;
      while ( it != _mapSessions.end() )
      {
         pInfo = it->second ;
         if ( pInfo->isIn() )
         {
            ++it ;
            continue ;
         }

         if ( pInfo->isValid()  )
         {
            pInfo->onTimer( interval ) ;
            if ( pInfo->isTimeout( OM_REST_SESSION_TIMEOUT ) )
            {
               pInfo->invalidate() ;
            }
         }

         if ( !pInfo->isValid() )
         {
            _delFromUserMap( pInfo->_attr._userName, pInfo ) ;
            SDB_OSS_DEL pInfo ;
            _mapSessions.erase( it++ ) ;
            continue ;
         }
         ++it ;
      }
      _omLatch.release() ;
   }

   restSessionInfo* _omManager::newSessionInfo( const string &userName,
                                                UINT32 localIP )
   {
      restSessionInfo *newSession = SDB_OSS_NEW restSessionInfo ;
      if( !newSession )
      {
         PD_LOG( PDERROR, "Alloc rest session info failed" ) ;
         goto error ;
      }

      _omLatch.get() ;
      newSession->_attr._sessionID = ossPack32To64( localIP, _sequence++ ) ;
      ossStrncpy( newSession->_attr._userName, userName.c_str(),
                  SESSION_USER_NAME_LEN ) ;
      _mapSessions[ _makeID( newSession ) ] = newSession ;
      _add2UserMap( userName, newSession ) ;
      newSession->_inNum.inc() ;
      _omLatch.release() ;

      if ( newSession )
      {
         newSession->lock() ;
      }

   done:
      return newSession ;
   error:
      goto done ;
   }

   void _omManager::releaseSessionInfo ( const string &sessionID )
   {
      restSessionInfo *pInfo = NULL ;
      map<string, restSessionInfo*>::iterator it ;

      _omLatch.get() ;
      it = _mapSessions.find( sessionID ) ;
      if ( it != _mapSessions.end() )
      {
         pInfo = it->second ;
         _delFromUserMap( pInfo->_attr._userName, pInfo ) ;

         if ( pInfo->isLock() )
         {
            detachSessionInfo( pInfo ) ;
         }

         if ( !pInfo->isIn() )
         {
            SDB_OSS_DEL pInfo ;
            _mapSessions.erase( it ) ;
         }
         else
         {
            _invalidSessionInfo( pInfo ) ;
         }
      }
      _omLatch.release() ;
   }

   void _omManager::_add2UserMap( const string &user,
                                  restSessionInfo *pSessionInfo )
   {
      map<string, vector<restSessionInfo*> >::iterator it ;
      it = _mapUser2Sessions.find( user ) ;
      if ( it == _mapUser2Sessions.end() )
      {
         vector<restSessionInfo*> vecSession ;
         vecSession.push_back( pSessionInfo ) ;
         _mapUser2Sessions.insert( make_pair( user, vecSession ) ) ;
      }
      else
      {
         it->second.push_back( pSessionInfo ) ;
      }
   }

   void _omManager::_delFromUserMap( const string &user,
                                     restSessionInfo *pSessionInfo )
   {
      map<string, vector<restSessionInfo*> >::iterator it ;
      it = _mapUser2Sessions.find( user ) ;
      if ( it != _mapUser2Sessions.end() )
      {
         vector<restSessionInfo*> &vecSessions = it->second ;
         vector<restSessionInfo*>::iterator itVec = vecSessions.begin() ;
         while ( itVec != vecSessions.end() )
         {
            if ( *itVec == pSessionInfo )
            {
               vecSessions.erase( itVec ) ;
               break ;
            }
            ++itVec ;
         }

         if ( vecSessions.size() == 0 )
         {
            _mapUser2Sessions.erase( it ) ;
         }
      }
   }

   string _omManager::_makeID( restSessionInfo * pSessionInfo )
   {
      UINT32 ip = 0 ;
      UINT32 seq = 0 ;
      ossUnpack32From64( pSessionInfo->_attr._sessionID, ip, seq ) ;
      CHAR tmp[9] = {0} ;
      ossSnprintf( tmp, sizeof(tmp)-1, "%08x", seq ) ;
      string strValue = md5::md5simpledigest( (const void*)pSessionInfo,
                                              pSessionInfo->getAttrSize() ) ;
      UINT32 size = strValue.size() ;
      strValue = strValue.substr( 0, size - ossStrlen( tmp ) ) ;
      strValue += tmp ;

      pSessionInfo->_id = strValue ;
      return strValue ;
   }

   netRouteAgent* _omManager::getRouteAgent()
   {
      return &_netAgent ;
   }

   MsgRouteID _omManager::_incNodeID()
   {
      ++_hwRouteID.columns.nodeID ;
      if ( 0 == _hwRouteID.columns.nodeID )
      {
         _hwRouteID.columns.nodeID = 1 ;
         ++_hwRouteID.columns.groupID ;
      }
      return _hwRouteID ;
   }

   MsgRouteID _omManager::updateAgentInfo( const string &host,
                                           const string &service )
   {
      MsgRouteID nodeID ;
      ossScopedLock lock( &_omLatch, EXCLUSIVE ) ;
      MAP_HOST2ID_IT it = _mapHost2ID.find( host ) ;
      if ( it != _mapHost2ID.end() )
      {
         omAgentInfo &info = it->second ;
         nodeID.value = info._id ;
         _netAgent.updateRoute( nodeID, host.c_str(), service.c_str() ) ;
         info._host = host ;
         info._service = service ;
      }
      else
      {
         nodeID = _incNodeID() ;
         omAgentInfo &info = _mapHost2ID[ host ] ;
         info._id = nodeID.value ;
         info._host = host ;
         info._service = service ;
         _mapID2Host[ info._id ] = &info ;
         _netAgent.updateRoute( nodeID, host.c_str(), service.c_str() ) ;
      }

      return nodeID ;
   }

   MsgRouteID _omManager::getAgentIDByHost( const string &host )
   {
      MsgRouteID nodeID ;
      nodeID.value = MSG_INVALID_ROUTEID ;
      ossScopedLock lock( &_omLatch, SHARED ) ;
      MAP_HOST2ID_IT it = _mapHost2ID.find( host ) ;
      if ( it != _mapHost2ID.end() )
      {
         nodeID.value = it->second._id ;
      }
      return nodeID ;
   }

   INT32 _omManager::getHostInfoByID( MsgRouteID routeID, string &host,
                                      string &service )
   {
      INT32 rc = SDB_OK ;
      omAgentInfo *pInfo = NULL ;

      ossScopedLock lock( &_omLatch, SHARED ) ;
      MAP_ID2HOSTPTR_IT it = _mapID2Host.find( routeID.value ) ;
      if ( it != _mapID2Host.end() )
      {
         pInfo = it->second ;
         host = pInfo->_host ;
         service = pInfo->_service ;
      }
      else
      {
         rc = SDB_CLS_NODE_NOT_EXIST ;
      }

      return rc ;
   }

   void _omManager::delAgent( const string &host )
   {
      ossScopedLock lock( &_omLatch, EXCLUSIVE ) ;
      MAP_HOST2ID_IT it = _mapHost2ID.find( host ) ;
      if ( it != _mapHost2ID.end() )
      {
         MsgRouteID nodeID ;
         nodeID.value = it->second._id ;
         _mapID2Host.erase( it->second._id ) ;
         _netAgent.delRoute( nodeID ) ;
         _mapHost2ID.erase( it ) ;
      }
   }

   void _omManager::delAgent( MsgRouteID routeID )
   {
      ossScopedLock lock( &_omLatch, EXCLUSIVE ) ;
      MAP_ID2HOSTPTR_IT it = _mapID2Host.find( routeID.value ) ;
      if ( it != _mapID2Host.end() )
      {
         MsgRouteID nodeID ;
         nodeID.value = it->first ;
         string host = it->second->_host ;
         _netAgent.delRoute( nodeID ) ;
         _mapID2Host.erase( it ) ;
         _mapHost2ID.erase( host ) ;
      }
   }

   string _omManager::getLocalAgentPort()
   {
      return _localAgentPort ;
   }

   void _omManager::_readAgentPort()
   {
      INT32 rc = SDB_OK ;
      CHAR conf[OSS_MAX_PATHSIZE + 1] = { 0 } ;
      po::options_description desc ( "Config options" ) ;
      po::variables_map vm ;
      CHAR hostport[OSS_MAX_HOSTNAME + 6] = { 0 } ;
      _localAgentPort = boost::lexical_cast<string>( SDBCM_DFT_PORT ) ;
      rc = ossGetHostName( hostport, OSS_MAX_HOSTNAME ) ;
      if ( rc != SDB_OK )
      {
         PD_LOG( PDERROR, "get host name failed:rc=%d", rc ) ;
         goto error ;
      }

      ossStrncat ( hostport, SDBCM_CONF_PORT, ossStrlen(SDBCM_CONF_PORT) ) ;

      desc.add_options()
         (SDBCM_CONF_DFTPORT, po::value<string>(), "sdbcm default "
         "listening port")
         (hostport, po::value<string>(), "sdbcm specified listening port")
      ;

      rc = ossGetEWD ( conf, OSS_MAX_PATHSIZE ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to get excutable file's working "
                  "directory" ) ;
         goto error ;
      }

      if ( ( ossStrlen ( conf ) + ossStrlen ( SDBCM_CONF_PATH_FILE ) + 2 ) >
           OSS_MAX_PATHSIZE )
      {
         PD_LOG ( PDERROR, "Working directory too long" ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }

      ossStrncat( conf, OSS_FILE_SEP, 1 );
      ossStrncat( conf, SDBCM_CONF_PATH_FILE,
                  ossStrlen( SDBCM_CONF_PATH_FILE ) );
      rc = utilReadConfigureFile ( conf, desc, vm ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Failed to read configure file, rc = %d", rc ) ;
         goto error ;
      }
      else if ( vm.count( hostport ) )
      {
         _localAgentPort = vm[hostport].as<string>() ;
      }
      else if ( vm.count( SDBCM_CONF_DFTPORT ) )
      {
         _localAgentPort = vm[SDBCM_CONF_DFTPORT].as<string>() ;
      }
      else
      {
         _localAgentPort = boost::lexical_cast<string>( SDBCM_DFT_PORT ) ;
      }

   done:
      return ;
   error:
      goto done ;
   }

   INT32 _omManager::_restoreTask()
   {
      INT32 rc         = SDB_OK ;
      rc = _taskManager->restoreTask() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "restore task failed:rc=%d", rc ) ;
         goto error ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   BOOLEAN _omManager::_isCommand( const CHAR *pCheckName )
   {
      if ( pCheckName && '$' == pCheckName[0] )
      {
         return TRUE ;
      }

      return FALSE ;
   }

   void _omManager::_sendRes2Agent( NET_HANDLE handle, MsgHeader *pSrcMsg, 
                                    INT32 flag, BSONObj response )
   {

      MsgOpReply reply ;
      INT32 rc                   = SDB_OK ;
      const CHAR *pBody          = response.objdata() ;
      INT32 bodyLen              = response.objsize() ;
      reply.header.messageLength = sizeof( MsgOpReply ) + bodyLen ;
      reply.header.opCode        = MAKE_REPLY_TYPE( pSrcMsg->opCode ) ;
      reply.header.TID           = pSrcMsg->TID ;
      reply.header.routeID.value = 0 ;
      reply.header.requestID     = pSrcMsg->requestID ;
      reply.contextID            = -1 ;
      reply.flags                = flag ;
      reply.startFrom            = 0 ;
      reply.numReturned          = 1 ;

      PD_LOG( PDERROR, "send response to agent:res=%s", 
              response.toString().c_str() ) ;
      if ( bodyLen > 0 )
      {
         rc = _netAgent.syncSend ( handle, (MsgHeader *)( &reply ),
                                   (void*)pBody, bodyLen ) ;
      }
      else
      {
         rc = _netAgent.syncSend ( handle, (void *)( &reply ) ) ;
      }

      if ( rc != SDB_OK )
      {
         PD_LOG ( PDERROR, "send response to agent failed:rc=%d", rc ) ;
      }
   }

   INT32 _omManager::_onAgentQueryTaskReq( NET_HANDLE handle, MsgHeader *pMsg )
   {
      INT32 rc = SDB_OK ;
      INT32 flags               = 0 ;
      CHAR *pCollectionName     = NULL ;
      CHAR *pQuery              = NULL ;
      CHAR *pFieldSelector      = NULL ;
      CHAR *pOrderByBuffer      = NULL ;
      CHAR *pHintBuffer         = NULL ;
      SINT64 numToSkip          = -1 ;
      SINT64 numToReturn        = -1 ;
      omAgentReqBase *pAgentReq = NULL ;
      rc = msgExtractQuery ( (CHAR *)pMsg, &flags, &pCollectionName,
                             &numToSkip, &numToReturn, &pQuery,
                             &pFieldSelector, &pOrderByBuffer, &pHintBuffer ) ;
      if ( rc )
      {
         PD_LOG_MSG( PDERROR, "extract omAgent's command msg failed:rc=%d", 
                     rc ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }

      PD_LOG( PDEVENT, "receive agent's command:%s", pCollectionName ) ;
      if ( _isCommand( pCollectionName ) )
      {
         BSONObj request( pQuery ) ;
         if ( ossStrcmp( pCollectionName + 1 , AGENT_QUERY_TASK_REQ ) == 0 )
         {
            pAgentReq = SDB_OSS_NEW agentQueryTaskReq( request, _taskManager) ;
         }
         else
         {
            rc = SDB_INVALIDARG ;
            PD_LOG_MSG( PDERROR, "unreconigzed agent request:command=%s", 
                        pCollectionName ) ;
            goto error ;
         }

         rc = pAgentReq->doCommand() ;
         if ( SDB_OK != rc )
         {
            PD_LOG_MSG( PDERROR, "process command failed:command=%s,rc=%d", 
                        pCollectionName, rc ) ;
            goto error ;
         }
      }
      else
      {
         rc = SDB_INVALIDARG ;
         PD_LOG_MSG( PDERROR, "unreconigzed agent request:command=%s", 
                     pCollectionName ) ;
         goto error ;
      }

   done:
      
      if ( NULL != pAgentReq && SDB_OK == rc )
      {
         BSONObj response ;
         pAgentReq->getResponse( response ) ;
         _sendRes2Agent( handle, pMsg, rc, response ) ;
      }
      else
      {
         BSONObj response ;
         string errorInfo = pmdGetThreadEDUCB()->getInfo( EDU_INFO_ERROR ) ;
         response = BSON( OP_ERR_DETAIL << errorInfo ) ;
         _sendRes2Agent( handle, pMsg, rc, response ) ;
      }

      if ( NULL != pAgentReq )
      {
         SDB_OSS_DEL pAgentReq ;
      }
      return rc ;
   error:
      goto done ;
   }

   /*
      get the global om manager object point
   */
   omManager* sdbGetOMManager()
   {
      static omManager s_omManager ;
      return &s_omManager ;
   }

}


