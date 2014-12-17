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

   Source File Name = pmdRestSession.cpp

   Dependencies: N/A

   Restrictions: N/A

   Change Activity:
   defect Date        Who Description
   ====== =========== === ==============================================
          04/14/2014  Xu Jianhui  Initial Draft

   Last Changed =

*******************************************************************************/

#include "pmdRestSession.hpp"
#include "pmdController.hpp"
#include "omManager.hpp"
#include "pmdEDUMgr.hpp"
#include "msgDef.h"
#include "utilCommon.hpp"
#include "ossMem.hpp"
#include "rtnCommand.hpp"
#include "../omsvc/omGetFileCommand.hpp"
#include "rtn.hpp"

#include "../bson/bson.h"

using namespace bson ;

namespace engine
{
   static void _sendOpError2Web ( INT32 rc, restAdaptor *pAdptor,
                                  pmdRestSession *pRestSession,
                                  pmdEDUCB* pEduCB ) ;

   void _sendOpError2Web ( INT32 rc, restAdaptor *pAdptor, 
                           pmdRestSession *pRestSession,
                           pmdEDUCB* pEduCB )
   {
      BSONObj _errorInfo = utilGetErrorBson( rc, pEduCB->getInfo( 
                                             EDU_INFO_ERROR ) ) ;
      pAdptor->setOPResult( pRestSession, rc, _errorInfo ) ;
      pAdptor->sendResponse( pRestSession, HTTP_OK ) ;
   }


   #define PMD_REST_SESSION_SNIFF_TIMEOUT    ( 10 * OSS_ONE_SEC )

   /*
      _restSessionInfo implement
   */
   void _restSessionInfo::releaseMem()
   {
      pmdEDUCB::CATCH_MAP_IT it = _catchMap.begin() ;
      while ( it != _catchMap.end() )
      {
         SDB_OSS_FREE( it->second ) ;
         ++it ;
      }
      _catchMap.clear() ;
   }

   void _restSessionInfo::pushMemToMap( _pmdEDUCB::CATCH_MAP &catchMap )
   {
      _pmdEDUCB::CATCH_MAP_IT it = _catchMap.begin() ;
      while ( it != _catchMap.end() )
      {
         catchMap.insert( std::make_pair( it->first, it->second ) ) ;
         ++it ;
      }
      _catchMap.clear() ;
   }

   void _restSessionInfo::makeMemFromMap( _pmdEDUCB::CATCH_MAP &catchMap )
   {
      _pmdEDUCB::CATCH_MAP_IT it = catchMap.begin() ;
      while ( it != catchMap.end() )
      {
         _catchMap.insert( std::make_pair( it->first, it->second ) ) ;
         ++it ;
      }
      catchMap.clear() ;
   }

   /*
      _pmdRestSession implement
   */
   _pmdRestSession::_pmdRestSession( SOCKET fd )
   :_pmdSession( fd )
   {
      _pFixBuff         = NULL ;
      _pSessionInfo     = NULL ;
      _pRTNCB           = NULL ;
      _pDPSCB           = NULL ;

      _wwwRootPath      = pmdGetOptionCB()->getWWWPath() ;
   }

   _pmdRestSession::~_pmdRestSession()
   {
      if ( _pFixBuff )
      {
         sdbGetPMDController()->releaseFixBuf( _pFixBuff ) ;
         _pFixBuff = NULL ;
      }
   }

   UINT64 _pmdRestSession::identifyID()
   {
      if ( _pSessionInfo )
      {
         return _pSessionInfo->_attr._sessionID ;
      }
      return 0 ;
   }

   INT32 _pmdRestSession::getServiceType() const
   {
      return CMD_SPACE_SERVICE_LOCAL ;
   }

   SDB_SESSION_TYPE _pmdRestSession::sessionType() const
   {
      return SDB_SESSION_REST ;
   }

   INT32 _pmdRestSession::run()
   {
      INT32 rc                         = SDB_OK ;
      restAdaptor *pAdptor             = sdbGetPMDController()->getRestAdptor() ;
      pmdEDUMgr *pEDUMgr               = NULL ;
      const CHAR *pSessionID           = NULL ;
      HTTP_PARSE_COMMON httpCommon     = COM_GETFILE ;
      CHAR *pFilePath                  = NULL ;
      INT32 bodySize                   = 0 ;

      if ( !_pEDUCB )
      {
         rc = SDB_SYS ;
         goto error ;
      }

      pEDUMgr = _pEDUCB->getEDUMgr() ;

      while ( !_pEDUCB->isDisconnected() && !_socket.isClosed() )
      {
         rc = sniffData( _pSessionInfo ? OSS_ONE_SEC :
                         PMD_REST_SESSION_SNIFF_TIMEOUT ) ;
         if ( SDB_TIMEOUT == rc )
         {
            if ( _pSessionInfo )
            {
               saveSession() ;
               sdbGetPMDController()->detachSessionInfo( _pSessionInfo ) ;
               _pSessionInfo = NULL ;
               continue ;
            }
            else
            {
               break ;
            }
         }
         else if ( rc < 0 )
         {
            break ;
         }

         if ( _pEDUCB->isInterrupted( TRUE ) )
         {
            INT64 contextID = -1 ;
            while ( -1 != ( contextID = _pEDUCB->contextPeek() ) )
            {
               _pRTNCB->contextDelete( contextID, NULL ) ;
            }
         }

         _pEDUCB->resetInterrupt() ;
         _pEDUCB->resetInfo( EDU_INFO_ERROR ) ;

         rc = pAdptor->recvRequestHeader( this ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Session[%s] failed to recv rest header, "
                    "rc: %d", sessionName(), rc ) ;
            if ( SDB_REST_EHS == rc )
            {
               pAdptor->sendResponse( this, HTTP_BADREQ ) ;
            }
            else if ( SDB_APP_FORCED != rc )
            {
               _sendOpError2Web( rc, pAdptor, this, _pEDUCB ) ;
            }
            break ;
         }
         if ( !_pSessionInfo )
         {
            pAdptor->getHttpHeader( this, FIELD_NAME_SESSIONID, &pSessionID ) ;
            if ( pSessionID )
            {
               PD_LOG( PDINFO, "Rest session: %s", pSessionID ) ;
               _pSessionInfo = sdbGetPMDController()->attachSessionInfo(
                                  pSessionID ) ;
            }

            if ( _pSessionInfo )
            {
               restoreSession() ;
            }
         }
         rc = pAdptor->recvRequestBody( this, httpCommon, &pFilePath, 
                                        bodySize ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Session[%s] failed to recv rest body, "
                    "rc: %d", sessionName(), rc ) ;
            if ( SDB_REST_EHS == rc )
            {
               pAdptor->sendResponse( this, HTTP_BADREQ ) ;
            }
            else if ( SDB_APP_FORCED != rc )
            {
               _sendOpError2Web( rc, pAdptor, this, _pEDUCB ) ;
            }
            break ;
         }

         if ( _pSessionInfo )
         {
            _pSessionInfo->active() ;
         }

         _pEDUCB->incEventCount() ;

         if ( SDB_OK != ( rc = pEDUMgr->activateEDU( _pEDUCB ) ) )
         {
            PD_LOG( PDERROR, "Session[%s] activate edu failed, rc: %d",
                    sessionName(), rc ) ;
            break ;
         }

         rc = _processRestMsg( httpCommon, pFilePath ) ;
         if ( rc )
         {
            break ;
         }

         if ( SDB_OK != ( rc = pEDUMgr->waitEDU( _pEDUCB ) ) )
         {
            PD_LOG( PDERROR, "Session[%s] wait edu failed, rc: %d",
                    sessionName(), rc ) ;
            break ;
         }

         if ( pFilePath )
         {
            releaseBuff( pFilePath, bodySize ) ;
            pFilePath = NULL ;
         }
         rc = SDB_OK ;
      } // end while

   done:
      if ( pFilePath )
      {
         releaseBuff( pFilePath, bodySize ) ;
      }
      disconnect() ;
      return rc ;
   error:
      goto done ;
   }

   INT32 _pmdRestSession::_processRestMsg( HTTP_PARSE_COMMON command, 
                                           const CHAR *pFilePath )
   {
      restAdaptor *pAdptor          = NULL ;
      omRestCommandBase *pOmCommand = NULL ;
      pAdptor = sdbGetPMDController()->getRestAdptor() ;
      pOmCommand = _createCommand( command, pFilePath ) ;
      if ( NULL == pOmCommand )
      {
         goto error ;
      }

      pOmCommand->init( _pEDUCB ) ;
      pOmCommand->doCommand() ;

   done:
      if ( NULL != pOmCommand )
      {
         SDB_OSS_DEL pOmCommand ;
      }
      return SDB_OK ;

   error:
      goto done ;
   }

   omRestCommandBase *_pmdRestSession::_createCommand( 
                                             HTTP_PARSE_COMMON command, 
                                             const CHAR *pFilePath )
   {
      omRestCommandBase *commandIf = NULL ;
      restAdaptor *pAdptor         = NULL ;
      CHAR hostName[ OSS_MAX_HOSTNAME + 1 ] ;
      pAdptor = sdbGetPMDController()->getRestAdptor() ;
      ossGetHostName( hostName, OSS_MAX_HOSTNAME ) ;
      string localAgentHost = hostName ;
      string localAgentPort = sdbGetOMManager()->getLocalAgentPort() ;

      if ( COM_GETFILE == command )
      {
         PD_LOG( PDEVENT, "OM: getfile command:file=%s", pFilePath ) ;
         commandIf = SDB_OSS_NEW omGetFileCommand( pAdptor, this,
                                                   _wwwRootPath.c_str(),
                                                   pFilePath ) ;
      }
      else 
      {
         const CHAR *pSubCommand = NULL ;
         pAdptor->getQuery( this, OM_REST_FIELD_COMMAND, &pSubCommand ) ;
         if ( NULL == pSubCommand )
         {
            BSONObjBuilder builder ;
            builder.append( OM_REST_RES_RETCODE, SDB_INVALIDARG ) ;
            builder.append( OM_REST_RES_DETAIL, "command is null" ) ;
            pAdptor->setOPResult( this, SDB_INVALIDARG, builder.obj() ) ;
            pAdptor->sendResponse( this, HTTP_OK ) ;
            goto error ;
         }

         PD_LOG( PDDEBUG, "OM: command:command=%s", pSubCommand ) ;
         if ( ossStrcmp( pSubCommand, OM_LOGIN_REQ ) != 0
              && ossStrcmp( pSubCommand, OM_CHECK_SESSION_REQ ) != 0
              && !isAuthOK() )
         {
            BSONObjBuilder builder ;
            builder.append( OM_REST_RES_RETCODE, 
                            SDB_AUTH_AUTHORITY_FORBIDDEN ) ;
            builder.append( OM_REST_RES_LOCAL, "/"OM_REST_LOGIN_HTML ) ;
            pAdptor->setOPResult( this, SDB_AUTH_AUTHORITY_FORBIDDEN, 
                                  builder.obj() ) ;
            pAdptor->sendResponse( this, HTTP_OK ) ;
            PD_LOG( PDEVENT, "OM: redirect to:%s", OM_REST_LOGIN_HTML ) ;
            goto error ;
         }
         
         if ( ossStrcasecmp( pSubCommand, OM_LOGIN_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omAuthCommand( pAdptor, this ) ;
         }
         else if( ossStrcasecmp( pSubCommand, OM_LOGOUT_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omLogoutCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_CHANGE_PASSWD_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omChangePasswdCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_CHECK_SESSION_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omCheckSessionCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_CREATE_CLUSTER_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omCreateClusterCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_QUERY_CLUSTER_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryClusterCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_SCAN_HOST_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omScanHostCommand( pAdptor, this, 
                                       localAgentHost, localAgentPort ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_CHECK_HOST_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omCheckHostCommand( pAdptor, this, 
                                       localAgentHost, localAgentPort ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_ADD_HOST_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omAddHostCommand( pAdptor, this, 
                                       localAgentHost, localAgentPort ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_LIST_HOST_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omListHostCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_QUERY_HOST_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryHostCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_QUERY_BUSINESS_TYPE_REQ ) 
                                                                          == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryBusinessTypeCommand( pAdptor, this, 
                                       _wwwRootPath.c_str(), pFilePath ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, 
                                  OM_QUERY_BUSINESS_TEMPLATE_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryBusinessTemplateCommand( pAdptor, 
                                       this, _wwwRootPath.c_str(), pFilePath ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_CONFIG_BUSINESS_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omConfigBusinessCommand( pAdptor, this, 
                                       _wwwRootPath.c_str(), pFilePath ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_INSTALL_BUSINESS_REQ) == 0 )
         {
            commandIf = SDB_OSS_NEW omInstallBusinessReq( pAdptor, this, 
                                       _wwwRootPath.c_str(), pFilePath, 
                                       localAgentHost, localAgentPort ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_QUERY_PROGRESS ) == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryInstallProgress( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_LIST_NODE_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omListNodeCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_QUERY_NODE_CONF_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryNodeConfCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_LIST_BUSINESS_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omListBusinessCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_QUERY_BUSINESS_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryBusinessCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_REMOVE_CLUSTER_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omRemoveClusterCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_REMOVE_HOST_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omRemoveHostCommand( pAdptor, this,
                                       localAgentHost, localAgentPort ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_REMOVE_BUSINESS_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omRemoveBusinessCommand( pAdptor, this,
                                       localAgentHost, localAgentPort ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_QUERY_HOST_STATUS_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryHostStatusCommand( pAdptor, this,
                                       localAgentHost, localAgentPort ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_PREDICT_CAPACITY_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omPredictCapacity( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_LIST_TASK_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omListTaskCommand( pAdptor, this ) ;
         }
         else if ( ossStrcasecmp( pSubCommand, OM_QUERY_TASK_REQ ) == 0 )
         {
            commandIf = SDB_OSS_NEW omQueryTaskCommand( pAdptor, this ) ;
         }
         else
         {
            BSONObjBuilder builder ;
            string errorInfo ;
            errorInfo = string("command is unreconigzed:") + pSubCommand ;
            builder.append( OM_REST_RES_RETCODE, SDB_INVALIDARG ) ;
            builder.append( OM_REST_RES_DETAIL, errorInfo.c_str() ) ;
            pAdptor->setOPResult( this, SDB_INVALIDARG, builder.obj() ) ;
            pAdptor->sendResponse( this, HTTP_OK ) ;
            goto error ;
         }
      }

   done:
      return commandIf ;
   error:
      goto done ;
   }

   void _pmdRestSession::_onAttach()
   {
      pmdKRCB *krcb = pmdGetKRCB() ;
      _pRTNCB = krcb->getRTNCB() ;
      _pDPSCB = krcb->getDPSCB() ;

      if ( _pDPSCB && !_pDPSCB->isLogLocal() )
      {
         _pDPSCB = NULL ;
      }
      sdbGetPMDController()->getRSManager()->registerEDU( eduCB() ) ;
   }

   void _pmdRestSession::_onDetach()
   {
      if ( DPS_INVALID_TRANS_ID != eduCB()->getTransID() )
      {
         INT32 rc = rtnTransRollback( eduCB(), _pDPSCB ) ;
         if ( rc )
         {
            PD_LOG( PDERROR, "Session[%s] rollback trans info failed, rc: %d",
                    sessionName(), rc ) ;
         }
      }

      INT64 contextID = -1 ;
      while ( -1 != ( contextID = eduCB()->contextPeek() ) )
      {
         _pRTNCB->contextDelete( contextID, NULL ) ;
      }

      eduCB()->setClientSock( NULL ) ;

      if ( _pSessionInfo )
      {
         saveSession() ;
         sdbGetPMDController()->detachSessionInfo( _pSessionInfo ) ;
         _pSessionInfo = NULL ;
      }

      sdbGetPMDController()->getRSManager()->unregEUD( eduCB() ) ;
   }

   INT32 _pmdRestSession::getFixBuffSize() const
   {
      return sdbGetPMDController()->getFixBufSize() ;
   }

   CHAR* _pmdRestSession::getFixBuff ()
   {
      if ( !_pFixBuff )
      {
         _pFixBuff = sdbGetPMDController()->allocFixBuf() ;
      }
      return _pFixBuff ;
   }

   void _pmdRestSession::restoreSession()
   {
      pmdEDUCB::CATCH_MAP catchMap ;
      _pSessionInfo->pushMemToMap( catchMap ) ;
      eduCB()->restoreBuffs( catchMap ) ;
   }

   void _pmdRestSession::saveSession()
   {
      pmdEDUCB::CATCH_MAP catchMap ;
      eduCB()->saveBuffs( catchMap ) ;
      _pSessionInfo->makeMemFromMap( catchMap ) ;
   }

   BOOLEAN _pmdRestSession::isAuthOK()
   {
      if ( NULL != _pSessionInfo )
      {
         if ( _pSessionInfo->_authOK )
         {
            return TRUE ;
         }
      }

      return FALSE ;
   }

   string _pmdRestSession::getLoginUserName()
   {
      if ( isAuthOK() )
      {
         return _pSessionInfo->_attr._userName ;
      }

      return "" ;
   }

   const CHAR* _pmdRestSession::getSessionID()
   {
      if ( NULL != _pSessionInfo )
      {
         if ( _pSessionInfo->_authOK )
         {
            return _pSessionInfo->_id.c_str();
         }
      }

      return "" ;
   }

   INT32 _pmdRestSession::doLogin( const string & username,
                                   UINT32 localIP )
   {
      INT32 rc = SDB_OK ;

      doLogout() ;

      _pSessionInfo = sdbGetPMDController()->newSessionInfo( username,
                                                             localIP ) ;
      if ( !_pSessionInfo )
      {
         rc = SDB_OOM ;
      }
      else
      {
         _pSessionInfo->_authOK = TRUE ;
      }
      return rc ;
   }

   void _pmdRestSession::doLogout()
   {
      if ( _pSessionInfo )
      {
         sdbGetPMDController()->releaseSessionInfo( _pSessionInfo->_id ) ;
         _pSessionInfo = NULL ;
      }
   }

}

