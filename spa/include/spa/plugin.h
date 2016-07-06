/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPA_PLUGIN_H__
#define __SPA_PLUGIN_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/props.h>

typedef struct _SpaHandle SpaHandle;
typedef struct _SpaHandleFactory SpaHandleFactory;

struct _SpaHandle {
  /* user_data that can be set by the application */
  void * user_data;
  /**
   * SpaHandle::get_interface:
   * @handle: a #SpaHandle
   * @interface_id: the interface id
   * @interface: result to hold the interface.
   *
   * Get the interface provided by @handle with @interface_id.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_NOT_IMPLEMENTED when there are no extensions
   *          #SPA_RESULT_INVALID_ARGUMENTS when handle or info is %NULL
   */
  SpaResult   (*get_interface)        (SpaHandle               *handle,
                                       uint32_t                 interface_id,
                                       const void             **interface);
};

/**
 * SpaInterfaceInfo:
 * @interface_id: the id of the interface, can be used to get the interface
 * @name: name of the interface
 * @description: Human readable description of the interface.
 *
 * This structure lists the information about available interfaces on
 * handles.
 */
typedef struct {
  uint32_t    interface_id;
  const char *name;
  const char *description;
} SpaInterfaceInfo;

struct _SpaHandleFactory {
  /**
   * SpaHandleFactory::name
   *
   * The name
   */
  const char * name;
  /**
   * SpaHandleFactory::info
   *
   * Extra information about the handles of this factory.
   */
  const SpaProps * info;

  /**
   * SpaHandleFactory::instantiate
   * @factory: a #SpaHandleFactory
   * @handle: a pointer to hold the result
   *
   * Make an instance of this factory.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_NOT_IMPLEMENTED when an instance can't be made
   *          #SPA_RESULT_INVALID_ARGUMENTS when factory or handle are %NULL
   */
  SpaResult   (*instantiate)          (const SpaHandleFactory  *factory,
                                       SpaHandle              **handle);
  /**
   * SpaHandle::enum_interface_info:
   * @factory: a #SpaHandleFactory
   * @index: the interface index
   * @info: result to hold SpaInterfaceInfo.
   *
   * Get the interface information at @index.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_NOT_IMPLEMENTED when there are no interfaces
   *          #SPA_RESULT_INVALID_ARGUMENTS when handle or info is %NULL
   *          #SPA_RESULT_ENUM_END when there are no more infos
   */
  SpaResult   (*enum_interface_info)  (const SpaHandleFactory  *factory,
                                       unsigned int             index,
                                       const SpaInterfaceInfo **info);
};

/**
 * SpaEnumHandleFactoryFunc:
 * @index: the index to enumerate
 * @factory: a location to hold the factory result
 *
 * The function signature of the entry point in a plugin.
 *
 * Returns: #SPA_RESULT_OK on success
 *          #SPA_RESULT_INVALID_ARGUMENTS when factory is %NULL
 *          #SPA_RESULT_ENUM_END when there are no more factories
 */
typedef SpaResult (*SpaEnumHandleFactoryFunc) (unsigned int             index,
                                               const SpaHandleFactory **factory);

/**
 * spa_enum_handle_factory:
 * @index: the index to enumerate
 * @factory: a location to hold the factory result
 *
 * The entry point in a plugin.
 *
 * Returns: #SPA_RESULT_OK on success
 *          #SPA_RESULT_INVALID_ARGUMENTS when factory is %NULL
 *          #SPA_RESULT_ENUM_END when there are no more factories
 */
SpaResult spa_enum_handle_factory       (unsigned int             index,
                                         const SpaHandleFactory **factory);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PLUGIN_H__ */