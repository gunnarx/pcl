/******************************************************************************
 * Project         Persistency
 * (c) copyright   2012
 * Company         XS Embedded GmbH
 *****************************************************************************/
/******************************************************************************
 * This Source Code Form is subject to the terms of the
 * Mozilla Public License, v. 2.0. If a  copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
******************************************************************************/
 /**
 * @file           persistence_client_library.c
 * @ingroup        Persistence client library
 * @author         Ingo Huerner
 * @brief          Implementation of the persistence client library.
 *                 Library provides an API to access persistent data
 * @see            
 */


#include "persistence_client_library_lc_interface.h"
#include "persistence_client_library_pas_interface.h"
#include "persistence_client_library_dbus_service.h"
#include "persistence_client_library_handle.h"
#include "persistence_client_library_custom_loader.h"
#include "persistence_client_library.h"
#include "persistence_client_library_backup_filelist.h"

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <dlfcn.h>

#include <dlt/dlt.h>
#include <dlt/dlt_common.h>

#include <dbus/dbus.h>

/// debug log and trace (DLT) setup
DLT_DECLARE_CONTEXT(gDLTContext);

// declared in persistence_client_library_dbus_service.c
// used to end dbus library
extern int bContinue;

static int gShutdownMode = 0;


int pclInitLibrary(const char* appName, int shutdownMode)
{
   int status = 0;
   int i = 0, rval = 1;

   if(gPclInitialized == PCLnotInitialized)
   {
      gShutdownMode = shutdownMode;

      DLT_REGISTER_CONTEXT(gDLTContext,"pers","Context for persistence client library logging");
      DLT_LOG(gDLTContext, DLT_LOG_INFO, DLT_STRING("pclInitLibrary => I N I T  Persistence Client Library - "), DLT_STRING(gAppId),
                           DLT_STRING("- init counter: "), DLT_INT(gPclInitialized) );

      /// environment variable for on demand loading of custom libraries
      const char *pOnDemandLoad = getenv("PERS_CUSTOM_LIB_LOAD_ON_DEMAND");
      /// environment variable for max key value data
      const char *pDataSize = getenv("PERS_MAX_KEY_VAL_DATA_SIZE");
      /// blacklist path environment variable
      const char *pBlacklistPath = getenv("PERS_BLACKLIST_PATH");

#if USE_PASINTERFACE == 1
      //printf("* ADMIN INTERFACE is  - e n a b l e d - \n");
      DLT_LOG(gDLTContext, DLT_LOG_INFO, DLT_STRING("PAS interface is enabled!!"));
#else
      //printf("* ADMIN INTERFACE is  - d i s a b l e d - enable with \"./configure --enable-pasinterface\"\n");
      DLT_LOG(gDLTContext, DLT_LOG_WARN, DLT_STRING("PAS interface is not enabled, enable with \"./configure --enable-pasinterface\""));
#endif


      pthread_mutex_lock(&gDbusPendingRegMtx);   // block until pending received

      if(pDataSize != NULL)
      {
         gMaxKeyValDataSize = atoi(pDataSize);
      }

      if(pBlacklistPath == NULL)
      {
         pBlacklistPath = "/etc/pclBackupBlacklist.txt";   // default path
      }

      if(readBlacklistConfigFile(pBlacklistPath) == -1)
      {
         DLT_LOG(gDLTContext, DLT_LOG_WARN, DLT_STRING("pclInitLibrary -> failed to access blacklist:"), DLT_STRING(pBlacklistPath));
      }

      if(setup_dbus_mainloop() == -1)
      {
         DLT_LOG(gDLTContext, DLT_LOG_ERROR, DLT_STRING("pclInitLibrary => Failed to setup main loop"));
         pthread_mutex_unlock(&gDbusPendingRegMtx);
         return EPERS_DBUS_MAINLOOP;
      }


      if(gShutdownMode != PCL_SHUTDOWN_TYPE_NONE)
      {
         // register for lifecycle and persistence admin service dbus messages
         if(register_lifecycle(shutdownMode) == -1)
         {
            DLT_LOG(gDLTContext, DLT_LOG_ERROR, DLT_STRING("pclInitLibrary => Failed to register to lifecycle dbus interface"));
            pthread_mutex_unlock(&gDbusPendingRegMtx);
            return EPERS_REGISTER_LIFECYCLE;
         }
      }
#if USE_PASINTERFACE == 1
      if(register_pers_admin_service() == -1)
      {
         DLT_LOG(gDLTContext, DLT_LOG_ERROR, DLT_STRING("pclInitLibrary => Failed to register to pers admin dbus interface"));
         pthread_mutex_unlock(&gDbusPendingRegMtx);
         return EPERS_REGISTER_ADMIN;
      }
#endif

      /// get custom library names to load
      status = get_custom_libraries();
      if(status >= 0)
      {
         // initialize custom library structure
         for(i = 0; i < PersCustomLib_LastEntry; i++)
         {
            invalidate_custom_plugin(i);
         }

         if(pOnDemandLoad == NULL)  // load all available libraries now
         {
            for(i=0; i < PersCustomLib_LastEntry; i++ )
            {
               if(check_valid_idx(i) != -1)
               {
                  if(load_custom_library(i, &gPersCustomFuncs[i] ) == 1)
                  {
                     if( (gPersCustomFuncs[i].custom_plugin_init) != NULL)
                     {
                        DLT_LOG(gDLTContext, DLT_LOG_INFO, DLT_STRING("pclInitLibrary => Loaded plugin: "),
                                                           DLT_STRING(get_custom_client_lib_name(i)));
                        gPersCustomFuncs[i].custom_plugin_init();
                     }
                     else
                     {
                        DLT_LOG(gDLTContext, DLT_LOG_ERROR, DLT_STRING("pclInitLibrary => E r r o r could not load plugin functions: "),
                                                            DLT_STRING(get_custom_client_lib_name(i)));
                     }
                  }
                  else
                  {
                     DLT_LOG(gDLTContext, DLT_LOG_ERROR, DLT_STRING("pclInitLibrary => E r r o r could not load plugin: "),
                                          DLT_STRING(get_custom_client_lib_name(i)));
                  }
               }
               else
               {
                  continue;
               }
            }
         }
      }
      else
      {
         DLT_LOG(gDLTContext, DLT_LOG_WARN, DLT_STRING("pclInit => Failed to load custom library config table => error number:"), DLT_INT(status));
      }

      // assign application name
      strncpy(gAppId, appName, MaxAppNameLen);
      gAppId[MaxAppNameLen-1] = '\0';

      // destory mutex
      pthread_mutex_destroy(&gDbusInitializedMtx);
      pthread_cond_destroy(&gDbusInitializedCond);

      gPclInitialized++;
   }
   else if(gPclInitialized >= PCLinitialized)
   {
      gPclInitialized++; // increment init counter
      DLT_LOG(gDLTContext, DLT_LOG_INFO, DLT_STRING("pclInitLibrary => I N I T  Persistence Client Library - "), DLT_STRING(gAppId),
                           DLT_STRING("- ONLY INCREMENT init counter: "), DLT_INT(gPclInitialized) );
   }

   return rval;
}



int pclDeinitLibrary(void)
{
   int i = 0, rval = 1;

   if(gPclInitialized == PCLinitialized)
   {
      DLT_LOG(gDLTContext, DLT_LOG_INFO, DLT_STRING("pclDeinitLibrary -> D E I N I T  client library - "), DLT_STRING(gAppId),
                                         DLT_STRING("- init counter: "), DLT_INT(gPclInitialized));
      // unregister for lifecycle and persistence admin service dbus messages
      if(gShutdownMode != PCL_SHUTDOWN_TYPE_NONE)
         rval = unregister_lifecycle(gShutdownMode);

#if USE_PASINTERFACE == 1
      rval = unregister_pers_admin_service();
#endif

      // unload custom client libraries
      for(i=0; i<PersCustomLib_LastEntry; i++)
      {
         if(gPersCustomFuncs[i].custom_plugin_deinit != NULL)
         {
            // deinitialize plugin
            gPersCustomFuncs[i].custom_plugin_deinit();
            // close library handle
            dlclose(gPersCustomFuncs[i].handle);

            invalidate_custom_plugin(i);
         }
      }

      // close all apend rct
      pers_rct_close_all();

      // close opend database
      pers_db_close_all();

      gPclInitialized = PCLnotInitialized;

      DLT_UNREGISTER_CONTEXT(gDLTContext);
   }
   else if(gPclInitialized > PCLinitialized)
   {
      DLT_LOG(gDLTContext, DLT_LOG_INFO, DLT_STRING("pclDeinitLibrary -> D E I N I T  client library - "), DLT_STRING(gAppId),
                                           DLT_STRING("- ONLY DECREMENT init counter: "), DLT_INT(gPclInitialized));
      gPclInitialized--;   // decrement init counter
   }
   else
   {
      rval = PCLnotInitialized;
   }

   // end dbus library
   bContinue = FALSE;

   pthread_mutex_destroy(&gDbusPendingRegMtx);
   return rval;
}






