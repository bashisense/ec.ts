/*
 * ec.ts
 *
 * Copyright (c) bashisense.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "private.h"
#include "utils.h"

#include <string.h>


typedef struct {
    JSContext *ctx;
    int closed;
    int finalized;
    uv_udp_t udp;
    struct {
        struct {
            JSValue tarray;
            uint8_t *data;
            size_t len;
        } b;
        TJSPromise result;
    } read;
} TJSUdp;

typedef struct {
    uv_udp_send_t req;
    TJSPromise result;
    JSValue tarray;
} TJSSendReq;

static JSClassID ects_http_class_id;

static void tjs_udp_finalizer(JSRuntime *rt, JSValue val) {
    TJSUdp *u = JS_GetOpaque(val, ects_http_class_id);
    if (u) {
        TJS_FreePromiseRT(rt, &u->read.result);
        JS_FreeValueRT(rt, u->read.b.tarray);
        u->finalized = 1;
        if (u->closed)
            free(u);
        else
            maybe_close(u);
    }
}

static void tjs_udp_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    TJSUdp *u = JS_GetOpaque(val, ects_http_class_id);
    if (u) {
        TJS_MarkPromise(rt, &u->read.result, mark_func);
        JS_MarkValue(rt, u->read.b.tarray, mark_func);
    }
}

static JSClassDef ects_http_class = {
    "UDP",
    .finalizer = tjs_udp_finalizer,
    .gc_mark = tjs_udp_mark,
};

static TJSUdp *tjs_udp_get(JSContext *ctx, JSValueConst obj) {
    return JS_GetOpaque2(ctx, obj, ects_http_class_id);
}

static JSValue tjs_new_udp(JSContext *ctx, int af) {
    TJSUdp *u;
    JSValue obj;
    int r;

    obj = JS_NewObjectClass(ctx, ects_http_class_id);
    if (JS_IsException(obj))
        return obj;

    u = calloc(1, sizeof(*u));
    if (!u) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    r = uv_udp_init_ex(tjs_get_loop(ctx), &u->udp, af);
    if (r != 0) {
        JS_FreeValue(ctx, obj);
        free(u);
        return JS_ThrowInternalError(ctx, "couldn't initialize UDP handle");
    }

    u->ctx = ctx;
    u->closed = 0;
    u->finalized = 0;

    u->udp.data = u;

    u->read.b.tarray = JS_UNDEFINED;
    u->read.b.data = NULL;
    u->read.b.len = 0;

    TJS_ClearPromise(ctx, &u->read.result);

    JS_SetOpaque(obj, u);
    return obj;
}

static JSValue ects_http_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    int af = AF_UNSPEC;
    if (!JS_IsUndefined(argv[0]) && JS_ToInt32(ctx, &af, argv[0]))
        return JS_EXCEPTION;
    return tjs_new_udp(ctx, af);
}

static JSValue tjs_udp_connect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    TJSUdp *u = tjs_udp_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;

    struct sockaddr_storage ss;
    int r;
    r = tjs_obj2addr(ctx, argv[0], &ss);
    if (r != 0)
        return JS_EXCEPTION;

    r = uv_udp_connect(&u->udp, (struct sockaddr *) &ss);
    if (r != 0)
        return tjs_throw_errno(ctx, r);

    return JS_UNDEFINED;
}

static JSValue tjs_udp_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    TJSUdp *u = tjs_udp_get(ctx, this_val);
    if (!u)
        return JS_EXCEPTION;

    struct sockaddr_storage ss;
    int r;
    r = tjs_obj2addr(ctx, argv[0], &ss);
    if (r != 0)
        return JS_EXCEPTION;

    int flags = 0;
    if (!JS_IsUndefined(argv[1]) && JS_ToInt32(ctx, &flags, argv[1]))
        return JS_EXCEPTION;

    r = uv_udp_bind(&u->udp, (struct sockaddr *) &ss, flags);
    if (r != 0)
        return tjs_throw_errno(ctx, r);

    return JS_UNDEFINED;
}

static const JSCFunctionListEntry ects_http_proto_funcs[] = {
    TJS_CFUNC_DEF("close", 0, tjs_udp_close),
    TJS_CFUNC_DEF("recv", 1, tjs_udp_recv),
    TJS_CFUNC_DEF("send", 2, tjs_udp_send),
    TJS_CFUNC_DEF("fileno", 0, tjs_udp_fileno),
    JS_CFUNC_MAGIC_DEF("getsockname", 0, tjs_udp_getsockpeername, 0),
    JS_CFUNC_MAGIC_DEF("getpeername", 0, tjs_udp_getsockpeername, 1),
    TJS_CFUNC_DEF("connect", 1, tjs_udp_connect),
    TJS_CFUNC_DEF("bind", 2, tjs_udp_bind),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "UDP", JS_PROP_CONFIGURABLE),
};

static const JSCFunctionListEntry tjs_udp_funcs[] = {
    TJS_UVCONST(UDP_IPV6ONLY),
    TJS_UVCONST(UDP_REUSEADDR),
};

void ects__mod_http_init(JSContext *ctx, JSValue ns) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto, obj;

    /* HTTP class */
    JS_NewClassID(rt, &ects_http_class_id);
    JS_NewClass(rt, ects_http_class_id, &ects_http_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, ects_http_proto_funcs, countof(ects_http_proto_funcs));
    JS_SetClassProto(ctx, ects_http_class_id, proto);

    /* HTTP object */
    obj = JS_NewCFunction2(ctx, ects_http_constructor, "HTTP", 1, JS_CFUNC_constructor, 0);
    JS_DefinePropertyValueStr(ctx, ns, "HTTP", obj, JS_PROP_C_W_E);

    JS_SetPropertyFunctionList(ctx, ns, tjs_udp_funcs, countof(tjs_udp_funcs));
}
