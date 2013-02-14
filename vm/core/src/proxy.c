/*
 * Copyright (C) 2012 Trillian AB
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <robovm.h>
#include <unwind.h>
#include "private.h"
#include "uthash.h"
#include "utlist.h"

#define ALLOC_PROXY_FRAMES_SIZE 2

typedef struct {
    const char* name;
    const char* desc;
} LookupKey;

typedef struct {
    LookupKey key;
    ProxyMethod* method;
    UT_hash_handle hh;
} LookupEntry;

typedef struct {
    LookupEntry* lookupsHash;
    ProxyHandler handler;
} ProxyClassData;

static ProxyMethod* hasMethod(Env* env, Class* clazz, const char* name, const char* desc) {
    char* paramsEnd = strchr(desc, ')');
    assert(paramsEnd != NULL);
    jint paramsLength = paramsEnd - desc + 1;

    MethodTable* methods = clazz->_methodTable;

    int i;
    for (i = 0; i < methods->size; i++) {
        MethodTableSlot * slot = &methods->slot[i];
        Method * method = &slot->method;

        if (method->name == NULL) continue;

        if (!strcmp(method->name, name) && !strncmp(method->desc, desc, paramsLength)) {
            return (ProxyMethod*) method;
        }
    }
    return NULL;
}

/**
 * Checks that a proxy method with the same name and parameter types as a method
 * have compatible return types.
 */
static jboolean checkCompatible(Env* env, Class* proxyClass, ProxyMethod* proxyMethod, Method* method, Class** t1, Class** t2) {
    // method's return type must be more specific that that of proxyMethod
    ClassLoader* classLoader = proxyClass->classLoader;
    Class* proxyRetType = rvmFindClassByDescriptor(env, rvmGetReturnType(proxyMethod->method.desc), classLoader);
    if (rvmExceptionClear(env)) {
        goto incompatible;
    }
    Class* methodRetType = rvmFindClassByDescriptor(env, rvmGetReturnType(method->desc), classLoader);
    if (rvmExceptionClear(env)) {
        goto incompatible;
    }
    *t1 = proxyRetType;
    *t2 = methodRetType;
    if (CLASS_IS_PRIMITIVE(methodRetType) || CLASS_IS_PRIMITIVE(proxyRetType)) {
        // If the return type of any of the methods is a primitive type or void,
        // then all of the methods must have that same return type.
        if (methodRetType != proxyRetType) {
            goto incompatible;
        }
    }
    if (!rvmIsAssignableFrom(env, proxyRetType, methodRetType) && !rvmIsAssignableFrom(env, methodRetType, proxyRetType)) {
        // Otherwise, one of the methods must have a return type that is assignable 
        // to all of the return types of the rest of the methods.
        goto incompatible;
    }
    return TRUE;
incompatible:
    rvmThrowNewf(env, java_lang_IllegalArgumentException, "Incompatible methods %s.%s%s and %s.%s%s", 
        rvmToBinaryClassName(env, proxyMethod->proxiedMethod->clazz->name),
        proxyMethod->method.name, proxyMethod->method.desc, 
        rvmToBinaryClassName(env, method->clazz->name),
        method->name, method->desc);
    return FALSE;
}

static jboolean tryAddProxyMethod(Env* env, Class* proxyClass, Method* method, jint access, ProxyClassData* proxyClassData) {
    ProxyMethod* proxyMethod = hasMethod(env, proxyClass, method->name, method->desc);
    if (rvmExceptionOccurred(env)) return FALSE;
    if (!proxyMethod) {
        proxyMethod = addProxyMethod(env, proxyClass, method, access, _proxy0);
        if (!proxyMethod) return FALSE;
        ObjectArray* exceptionTypes = rvmAttributeGetExceptions(env, method);        
        if (!exceptionTypes) return FALSE;
        jint i;
        for (i = exceptionTypes->length - 1; i >= 0; i--) {
            ProxyMethodException* pme = rvmAllocateMemory(env, sizeof(ProxyMethodException));
            if (!pme) {
                return FALSE;
            }
            pme->clazz = (Class*) exceptionTypes->values[i];
            LL_PREPEND(proxyMethod->allowedExceptions, pme);
        }
    } else {
        Class* t1;
        Class* t2;
        if (!checkCompatible(env, proxyClass, proxyMethod, method, &t1, &t2)) {
            return FALSE;
        }
        // t1 is the current return type of proxyMethod. If t2 
        // is more specific then use that instead.
        if (!CLASS_IS_PRIMITIVE(t1) && rvmIsAssignableFrom(env, t2, t1)) {
            proxyMethod->method.desc = method->desc;
        }

        // Remove exceptions from the proxied method that aren't thrown by all methods
        ObjectArray* exceptionTypes = rvmAttributeGetExceptions(env, method);        
        if (!exceptionTypes) return FALSE;
        ProxyMethodException* pme = NULL;
        ProxyMethodException* tmp = NULL;
        LL_FOREACH_SAFE(proxyMethod->allowedExceptions, pme, tmp) {
            Class* matchExClazz = NULL;
            jint i;
            for (i = 0; i < exceptionTypes->length; i++) {
                Class* exClazz = (Class*) exceptionTypes->values[i];
                if (rvmIsSubClass(pme->clazz, exClazz)) {
                    // exClazz is compatible with pme->clazz. Don't delete.
                    if (!matchExClazz || rvmIsSubClass(exClazz, matchExClazz)) {
                        // The previously found compatible exception class is 
                        // equal to or more specific than this one. Use this one.
                        matchExClazz = exClazz;
                    }
                }
            }
            if (matchExClazz) {
                pme->clazz = matchExClazz;
            } else {
                LL_DELETE(proxyMethod->allowedExceptions, pme);
            }
        }
    }
    // Record the lookup function in proxyClassData
    LookupEntry* entry = rvmAllocateMemory(env, sizeof(LookupEntry));
    if (!entry) return FALSE;
    entry->key.name = method->name;
    entry->key.desc = method->desc;
    entry->method = proxyMethod;
    HASH_ADD(hh, proxyClassData->lookupsHash, key, sizeof(LookupKey), entry);
    return TRUE;
}

static jboolean addProxyMethods(Env* env, Class* proxyClass, Class* clazz, ProxyClassData* proxyClassData) {
    // Add constructors from the super class and override all overridable methods. Constructors will use 
    // the same impl as the superclass. Overridden methods will have _rvmProxy0 as its impl.

    if (clazz->superclass) {
        if (!addProxyMethods(env, proxyClass, clazz->superclass, proxyClassData)) return FALSE;
    }

    MethodTable* methods = rvmGetMethods(env, clazz);
    if (rvmExceptionOccurred(env)) return FALSE;

    int i;
    for (i = 0; i < methods->size; i++) {
        MethodTableSlot * slot = &methods->slot[i];
        Method * method = &slot->method;

        if (method->name == NULL) continue;

        if (!METHOD_IS_STATIC(method) && !METHOD_IS_PRIVATE(method) && !METHOD_IS_FINAL(method)
                && (!METHOD_IS_CONSTRUCTOR(method) || clazz == proxyClass->superclass)) {

            if (METHOD_IS_CONSTRUCTOR(method)) {
                // TODO: For now we make all constructors public to satisfy java.lang.reflect.Proxy. 
                if (!addProxyMethod(env, proxyClass, method, ACC_PUBLIC, method->impl)) return FALSE;
            } else {
                if (METHOD_IS_PUBLIC(method)) { 
                    jint access = (method->access & (~ACC_ABSTRACT & ~ACC_NATIVE)) | ACC_FINAL;
                    tryAddProxyMethod(env, proxyClass, method, access, proxyClassData);
                }
            }
        }
    }

    return TRUE;
}

/**
 * Implements all abstract methods in {@code proxyClass}. This should be called after
 * {@link #addProxyMethods()} which will override all methods defined by the proxy's
 * ancestor classes (abstract or concrete).
 */
static jboolean implementAbstractInterfaceMethods(Env* env, Class* proxyClass, Interface* interface, ProxyClassData* proxyClassData) {
    if (!interface) return TRUE;

    MethodTable* methods = rvmGetMethods(env, interface->interface);
    if (rvmExceptionOccurred(env)) return FALSE;

    int i;
    for (i = 0; i < methods->size; i++) {
        MethodTableSlot * slot = &methods->slot[i];
        Method * method = &slot->method;

        if (method->name == NULL) continue;

        if (!METHOD_IS_CLASS_INITIALIZER(method)) {
            jint access = (method->access & (~ACC_ABSTRACT)) | ACC_FINAL;
            tryAddProxyMethod(env, proxyClass, method, access, proxyClassData);
        }
    }

    // Interfaces are implemented depth-first so call recursively with the super interfaces of the current interface first
    Interface* interfaceInterfaces = rvmGetInterfaces(env, interface->interface);
    if (rvmExceptionCheck(env)) return FALSE;
    if (!implementAbstractInterfaceMethods(env, proxyClass, interfaceInterfaces, proxyClassData)) return FALSE;
    // Now proceed with the next interface
    if (!implementAbstractInterfaceMethods(env, proxyClass, interface->next, proxyClassData)) return FALSE;

    return TRUE;
}

Class* rvmProxyCreateProxyClass(Env* env, Class* superclass, ClassLoader* classLoader, char* className, jint interfacesCount, Class** interfaces, 
        jint instanceDataSize, jint instanceDataOffset, unsigned short instanceRefCount, ProxyHandler handler) {

    // Allocate the proxy class.
    Class* proxyClass = rvmAllocateClass(env, className, superclass, classLoader, CLASS_TYPE_PROXY | ACC_PUBLIC | ACC_FINAL, 
        offsetof(Class, data) + sizeof(ProxyClassData), instanceDataSize, instanceDataOffset, 1, instanceRefCount, NULL, NULL);
    if (!proxyClass) return NULL;

    ProxyClassData* proxyClassData = (ProxyClassData*) proxyClass->data;
    proxyClassData->handler = handler;

    // Add interfaces
    jint i;
    for (i = 0; i < interfacesCount; i++) {
        if (!rvmAddInterface(env, proxyClass, (Class*) interfaces[i])) return NULL;
    }

    // Initialize methods to NULL to prevent rvmGetMethods() from trying to load the methods if called with this proxy class
    proxyClass->_methodTable = rvmBuildMethodTable(env, 64);

    if (!addProxyMethods(env, proxyClass, superclass, proxyClassData)) return NULL;

    Class* c = proxyClass;
    while (c) {
        Interface* interface = rvmGetInterfaces(env, c);
        if (rvmExceptionCheck(env)) return NULL;
        if (!implementAbstractInterfaceMethods(env, proxyClass, interface, proxyClassData)) return NULL;
        c = c->superclass;
    }

    if (!rvmRegisterClass(env, proxyClass)) return NULL;

    return proxyClass;
}

void _rvmProxyHandler(CallInfo* callInfo) {
    Env* env = (Env*) proxy0NextPtr(callInfo);
    Object* receiver = (Object*) proxy0NextPtr(callInfo);
    Class* proxyClass = receiver->clazz;
    ProxyClassData* proxyClassData = (ProxyClassData*) proxyClass->data;

    LookupKey key;
    memset(&key, 0, sizeof(LookupKey));
    key.name = (char*) env->reserved0;
    key.desc = (char*) env->reserved1;
    LookupEntry* entry;
    HASH_FIND(hh, proxyClassData->lookupsHash, &key, sizeof(LookupKey), entry);
    if (!entry) {
        rvmThrowNoSuchMethodError(env, "Failed to determine which method was called on proxy class");
        goto error;
    }

    ProxyMethod* method = entry->method;

    rvmPushGatewayFrameProxy(env, method);

    jint argsCount = rvmGetParameterCount((Method*) method);
    jvalue *jvalueArgs = NULL;
    if (argsCount > 0) {
        jvalueArgs = (jvalue*) rvmAllocateMemory(env, sizeof(jvalue) * argsCount);
        if (!jvalueArgs) goto errorPop;

        const char* desc = method->method.desc;
        const char* c;
        jint i = 0;
        while ((c = rvmGetNextParameterType(&desc))) {
            switch (c[0]) {
            case 'B':
                jvalueArgs[i++].b = (jbyte) proxy0NextInt(callInfo);
                break;
            case 'Z':
                jvalueArgs[i++].z = (jboolean) proxy0NextInt(callInfo);
                break;
            case 'S':
                jvalueArgs[i++].s = (jshort) proxy0NextInt(callInfo);
                break;
            case 'C':
                jvalueArgs[i++].c = (jchar) proxy0NextInt(callInfo);
                break;
            case 'I':
                jvalueArgs[i++].i = proxy0NextInt(callInfo);
                break;
            case 'J':
                jvalueArgs[i++].j = proxy0NextLong(callInfo);
                break;
            case 'F':
                jvalueArgs[i++].f = proxy0NextFloat(callInfo);
                break;
            case 'D':
                jvalueArgs[i++].d = proxy0NextDouble(callInfo);
                break;
            case '[':
            case 'L':
                jvalueArgs[i++].l = (jobject) proxy0NextPtr(callInfo);
                break;
            }
        }
    }

    jvalue returnValue;
    proxyClassData->handler(env, receiver, method, jvalueArgs, &returnValue);

    rvmPopGatewayFrame(env);

    if (rvmExceptionCheck(env)) goto error;

    proxy0ReturnInt(callInfo, 0);
    switch (rvmGetReturnType(method->method.desc)[0]) {
    case 'B':
        proxy0ReturnInt(callInfo, (jint) returnValue.b);
        break;
    case 'Z':
        proxy0ReturnInt(callInfo, (jint) returnValue.z);
        break;
    case 'S':
        proxy0ReturnInt(callInfo, (jint) returnValue.s);
        break;
    case 'C':
        proxy0ReturnInt(callInfo, (jint) returnValue.c);
        break;
    case 'I':
        proxy0ReturnInt(callInfo, returnValue.i);
        break;
    case 'J':
        proxy0ReturnLong(callInfo, returnValue.j);
        break;
    case 'F':
        proxy0ReturnFloat(callInfo, returnValue.f);
        break;
    case 'D':
        proxy0ReturnDouble(callInfo, returnValue.d);
        break;
    case '[':
    case 'L':
        proxy0ReturnPtr(callInfo, returnValue.l);
        break;
    }

    return;

errorPop:
    rvmPopGatewayFrame(env);
error:
    rvmRaiseException(env, rvmExceptionOccurred(env));
}

