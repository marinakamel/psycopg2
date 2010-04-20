/* connection_type.c - python interface to connection objects
 *
 * Copyright (C) 2003-2010 Federico Di Gregorio <fog@debian.org>
 *
 * This file is part of psycopg.
 *
 * psycopg2 is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link this program with the OpenSSL library (or with
 * modified versions of OpenSSL that use the same license as OpenSSL),
 * and distribute linked combinations including the two.
 *
 * You must obey the GNU Lesser General Public License in all respects for
 * all of the code used other than OpenSSL.
 *
 * psycopg2 is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>
#include <stringobject.h>

#include <string.h>
#include <ctype.h>

#define PSYCOPG_MODULE
#include "psycopg/config.h"
#include "psycopg/python.h"
#include "psycopg/psycopg.h"
#include "psycopg/connection.h"
#include "psycopg/cursor.h"
#include "psycopg/pqpath.h"
#include "psycopg/lobject.h"

/** DBAPI methods **/

/* cursor method - allocate a new cursor */

#define psyco_conn_cursor_doc \
"cursor(cursor_factory=extensions.cursor) -- new cursor\n\n"                \
"Return a new cursor.\n\nThe ``cursor_factory`` argument can be used to\n"  \
"create non-standard cursors by passing a class different from the\n"       \
"default. Note that the new class *should* be a sub-class of\n"             \
"`extensions.cursor`.\n\n"                                                  \
":rtype: `extensions.cursor`"

static PyObject *
psyco_conn_cursor(connectionObject *self, PyObject *args, PyObject *keywds)
{
    const char *name = NULL;
    PyObject *obj, *factory = NULL;

    static char *kwlist[] = {"name", "cursor_factory", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, keywds, "|sO", kwlist,
                                     &name, &factory)) {
        return NULL;
    }

    EXC_IF_CONN_CLOSED(self);

    if (self->status != CONN_STATUS_READY &&
        self->status != CONN_STATUS_BEGIN) {
        PyErr_SetString(OperationalError,
                        "asynchronous connection attempt underway");
        return NULL;
    }

    if (name != NULL && self->async == 1) {
        PyErr_SetString(ProgrammingError,
                        "asynchronous connections "
                        "cannot produce named cursors");
        return NULL;
    }

    Dprintf("psyco_conn_cursor: new cursor for connection at %p", self);
    Dprintf("psyco_conn_cursor:     parameters: name = %s", name);

    if (factory == NULL) factory = (PyObject *)&cursorType;
    if (name)
        obj = PyObject_CallFunction(factory, "Os", self, name);
    else
        obj = PyObject_CallFunction(factory, "O", self);

    if (obj == NULL) return NULL;
    if (PyObject_IsInstance(obj, (PyObject *)&cursorType) == 0) {
        PyErr_SetString(PyExc_TypeError,
            "cursor factory must be subclass of psycopg2._psycopg.cursor");
        Py_DECREF(obj);
        return NULL;
    }

    Dprintf("psyco_conn_cursor: new cursor at %p: refcnt = "
        FORMAT_CODE_PY_SSIZE_T,
        obj, obj->ob_refcnt
      );
    return obj;
}


/* close method - close the connection and all related cursors */

#define psyco_conn_close_doc "close() -- Close the connection."

static PyObject *
psyco_conn_close(connectionObject *self, PyObject *args)
{
    EXC_IF_CONN_CLOSED(self);

    if (!PyArg_ParseTuple(args, "")) return NULL;

    Dprintf("psyco_conn_close: closing connection at %p", self);
    conn_close(self);
    Dprintf("psyco_conn_close: connection at %p closed", self);

    Py_INCREF(Py_None);
    return Py_None;
}


/* commit method - commit all changes to the database */

#define psyco_conn_commit_doc "commit() -- Commit all changes to database."

static PyObject *
psyco_conn_commit(connectionObject *self, PyObject *args)
{
    EXC_IF_CONN_CLOSED(self);
    EXC_IF_CONN_ASYNC(self, commit);

    if (!PyArg_ParseTuple(args, "")) return NULL;

    if (conn_commit(self) < 0)
        return NULL;

    Py_INCREF(Py_None);
    return Py_None;
}


/* rollback method - roll back all changes done to the database */

#define psyco_conn_rollback_doc \
"rollback() -- Roll back all changes done to database."

static PyObject *
psyco_conn_rollback(connectionObject *self, PyObject *args)
{
    EXC_IF_CONN_CLOSED(self);
    EXC_IF_CONN_ASYNC(self, rollback);

    if (!PyArg_ParseTuple(args, "")) return NULL;

    if (conn_rollback(self) < 0)
        return NULL;

    Py_INCREF(Py_None);
    return Py_None;
}


#ifdef PSYCOPG_EXTENSIONS

/* set_isolation_level method - switch connection isolation level */

#define psyco_conn_set_isolation_level_doc \
"set_isolation_level(level) -- Switch isolation level to ``level``."

static PyObject *
psyco_conn_set_isolation_level(connectionObject *self, PyObject *args)
{
    int level = 1;

    EXC_IF_CONN_CLOSED(self);
    EXC_IF_CONN_ASYNC(self, set_isolation_level);

    if (!PyArg_ParseTuple(args, "i", &level)) return NULL;

    if (level < 0 || level > 2) {
        PyErr_SetString(PyExc_ValueError,
                        "isolation level out of bounds (0,3)");
        return NULL;
    }

    if (conn_switch_isolation_level(self, level) < 0) {
        PyErr_SetString(OperationalError,
                        PQerrorMessage(self->pgconn));
        return NULL;
    }


    Py_INCREF(Py_None);
    return Py_None;
}

/* set_client_encoding method - set client encoding */

#define psyco_conn_set_client_encoding_doc \
"set_client_encoding(encoding) -- Set client encoding to ``encoding``."

static PyObject *
psyco_conn_set_client_encoding(connectionObject *self, PyObject *args)
{
    const char *enc = NULL;
    char *buffer;
    size_t i, j;

    EXC_IF_CONN_CLOSED(self);
    EXC_IF_CONN_ASYNC(self, set_client_encoding);

    if (!PyArg_ParseTuple(args, "s", &enc)) return NULL;

    /* convert to upper case and remove '-' and '_' from string */
    buffer = PyMem_Malloc(strlen(enc)+1);
    for (i=j=0 ; i < strlen(enc) ; i++) {
        if (enc[i] == '_' || enc[i] == '-')
            continue;
        else
            buffer[j++] = toupper(enc[i]);
    }
    buffer[j] = '\0';

    if (conn_set_client_encoding(self, buffer) == 0) {
        PyMem_Free(buffer);
        Py_INCREF(Py_None);
        return Py_None;
    }
    else {
        PyMem_Free(buffer);
        return NULL;
    }
}

/* get_transaction_status method - Get backend transaction status */

#define psyco_conn_get_transaction_status_doc \
"get_transaction_status() -- Get backend transaction status."

static PyObject *
psyco_conn_get_transaction_status(connectionObject *self, PyObject *args)
{
    EXC_IF_CONN_CLOSED(self);

    if (!PyArg_ParseTuple(args, "")) return NULL;

    return PyInt_FromLong((long)PQtransactionStatus(self->pgconn));
}

/* get_parameter_status method - Get server parameter status */

#define psyco_conn_get_parameter_status_doc \
"get_parameter_status(parameter) -- Get backend parameter status.\n\n" \
"Potential values for ``parameter``:\n" \
"  server_version, server_encoding, client_encoding, is_superuser,\n" \
"  session_authorization, DateStyle, TimeZone, integer_datetimes,\n" \
"  and standard_conforming_strings\n" \
"If server did not report requested parameter, None is returned.\n\n" \
"See libpq docs for PQparameterStatus() for further details."

static PyObject *
psyco_conn_get_parameter_status(connectionObject *self, PyObject *args)
{
    const char *param = NULL;
    const char *val = NULL;

    EXC_IF_CONN_CLOSED(self);

    if (!PyArg_ParseTuple(args, "s", &param)) return NULL;

    val = PQparameterStatus(self->pgconn, param);
    if (!val) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    return PyString_FromString(val);
}


/* lobject method - allocate a new lobject */

#define psyco_conn_lobject_doc \
"cursor(oid=0, mode=0, new_oid=0, new_file=None,\n"                         \
"       lobject_factory=extensions.lobject) -- new lobject\n\n"             \
"Return a new lobject.\n\nThe ``lobject_factory`` argument can be used\n"   \
"to create non-standard lobjects by passing a class different from the\n"   \
"default. Note that the new class *should* be a sub-class of\n"             \
"`extensions.lobject`.\n\n"                                                 \
":rtype: `extensions.lobject`"

static PyObject *
psyco_conn_lobject(connectionObject *self, PyObject *args, PyObject *keywds)
{
    Oid oid=InvalidOid, new_oid=InvalidOid;
    char *smode = NULL, *new_file = NULL;
    int mode=0;
    PyObject *obj, *factory = NULL;

    static char *kwlist[] = {"oid", "mode", "new_oid", "new_file",
                             "cursor_factory", NULL};
    
    if (!PyArg_ParseTupleAndKeywords(args, keywds, "|izizO", kwlist,
                                     &oid, &smode, &new_oid, &new_file, 
                                     &factory)) {
        return NULL;
    }

    EXC_IF_CONN_CLOSED(self);
    EXC_IF_CONN_ASYNC(self, lobject);

    Dprintf("psyco_conn_lobject: new lobject for connection at %p", self);
    Dprintf("psyco_conn_lobject:     parameters: oid = %d, mode = %s",
            oid, smode);
    Dprintf("psyco_conn_lobject:     parameters: new_oid = %d, new_file = %s",
            new_oid, new_file);
    
    /* build a mode number out of the mode string: right now we only accept
       'r', 'w' and 'rw' (but note that 'w' implies 'rw' because PostgreSQL
       backend does that. */
    if (smode) {
        if (strncmp("rw", smode, 2) == 0)
            mode = INV_READ+INV_WRITE;
        else if (smode[0] == 'r')
           mode = INV_READ;
        else if (smode[0] == 'w')
           mode = INV_WRITE;
        else if (smode[0] == 'n')
           mode = -1;
        else {
            PyErr_SetString(PyExc_TypeError,
                "mode should be one of 'r', 'w' or 'rw'");
            return NULL;
        }
    }

    if (factory == NULL) factory = (PyObject *)&lobjectType;
    if (new_file)
        obj = PyObject_CallFunction(factory, "Oiiis", 
            self, oid, mode, new_oid, new_file);
    else
        obj = PyObject_CallFunction(factory, "Oiii",
            self, oid, mode, new_oid);

    if (obj == NULL) return NULL;
    if (PyObject_IsInstance(obj, (PyObject *)&lobjectType) == 0) {
        PyErr_SetString(PyExc_TypeError,
            "lobject factory must be subclass of psycopg2._psycopg.lobject");
        Py_DECREF(obj);
        return NULL;
    }
    
    Dprintf("psyco_conn_lobject: new lobject at %p: refcnt = "
            FORMAT_CODE_PY_SSIZE_T,
            obj, obj->ob_refcnt);
    return obj;
}

/* get the current backend pid */

#define psyco_conn_get_backend_pid_doc \
"get_backend_pid() -- Get backend process id."

static PyObject *
psyco_conn_get_backend_pid(connectionObject *self)
{
    EXC_IF_CONN_CLOSED(self);

    return PyInt_FromLong((long)PQbackendPID(self->pgconn));
}

/* reset the currect connection */

#define psyco_conn_reset_doc \
"reset() -- Reset current connection to defaults."

static PyObject *
psyco_conn_reset(connectionObject *self)
{
    int res;

    EXC_IF_CONN_CLOSED(self);
    EXC_IF_CONN_ASYNC(self, reset);

    if (pq_reset(self) < 0)
        return NULL;

    res = conn_setup(self, self->pgconn);
    if (res < 0)
        return NULL;

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
psyco_conn_get_exception(PyObject *self, void *closure)
{
    PyObject *exception = *(PyObject **)closure;

    Py_INCREF(exception);
    return exception;
}

#define psyco_conn_poll_doc \
"poll() -- return POLL_OK if the operation has finished, "           \
 "POLL_READ if the application should be waiting "                   \
 "for the socket to be readable or POLL_WRITE "                      \
 "if the socket should be writable."

static PyObject *
psyco_conn_poll_async(connectionObject *self)
{
    PostgresPollingStatusType poll_status;

    Dprintf("conn_poll: polling with status %d", self->status);

    switch (self->status) {

    case CONN_STATUS_SETUP:
        /* according to libpq documentation the user should start by waiting
           for the socket to become writable */
        self->status = CONN_STATUS_ASYNC;
        return PyInt_FromLong(PSYCO_POLL_WRITE);

    case CONN_STATUS_SEND_DATESTYLE:
    case CONN_STATUS_SENT_DATESTYLE:
    case CONN_STATUS_SEND_CLIENT_ENCODING:
    case CONN_STATUS_SENT_CLIENT_ENCODING:
        /* these mean that we need to wait for the socket to become writable
           to send the rest of our query */
        return conn_poll_connect_send(self);

    case CONN_STATUS_GET_DATESTYLE:
    case CONN_STATUS_GET_CLIENT_ENCODING:
        /* these mean that we are waiting for the results of the queries */
        return conn_poll_connect_fetch(self);

    case CONN_STATUS_ASYNC:
        /* this means we are in the middle of a PQconnectPoll loop */
        break;

    case CONN_STATUS_READY:
    case CONN_STATUS_BEGIN:
        /* The connection is ready, but we might be in an asynchronous query,
           or we just might want to check for NOTIFYs.  For synchronous
           connections the status might be BEGIN, not READY. */
        return conn_poll_ready(self);

    default:
        /* everything else is an error */
        PyErr_SetString(OperationalError,
                        "not in asynchronous connection attempt");
        return NULL;

    }

    Py_BEGIN_ALLOW_THREADS;
    pthread_mutex_lock(&self->lock);

    poll_status = PQconnectPoll(self->pgconn);

    if (poll_status == PGRES_POLLING_READING) {
        pthread_mutex_unlock(&(self->lock));
        Py_BLOCK_THREADS;
        Dprintf("conn_poll: returing POLL_READ");
        return PyInt_FromLong(PSYCO_POLL_READ);
    }

    if (poll_status == PGRES_POLLING_WRITING) {
        pthread_mutex_unlock(&(self->lock));
        Py_BLOCK_THREADS;
        Dprintf("conn_poll: returing POLL_WRITE");
        return PyInt_FromLong(PSYCO_POLL_WRITE);
    }

    if (poll_status == PGRES_POLLING_FAILED) {
        pthread_mutex_unlock(&(self->lock));
        Py_BLOCK_THREADS;
        PyErr_SetString(OperationalError, PQerrorMessage(self->pgconn));
        return NULL;
    }

    /* the only other thing that PQconnectPoll can return is PGRES_POLLING_OK,
       but make sure */
    if (poll_status != PGRES_POLLING_OK) {
        pthread_mutex_unlock(&(self->lock));
        Py_BLOCK_THREADS;
        PyErr_Format(OperationalError,
                     "unexpected result from PQconnectPoll: %d", poll_status);
        return NULL;
    }

    Dprintf("conn_poll: got POLL_OK");

    /* the connection is built, but we want to do a few other things before we
       let the user use it */

    self->equote = conn_get_standard_conforming_strings(self->pgconn);

    Dprintf("conn_poll: got standard_conforming_strings");

    /*
     * Here is the tricky part, we need to figure the datestyle,
     * client_encoding and isolevel, all using nonblocking calls.  To do that
     * we will keep telling the user to poll, while we are waiting for our
     * asynchronous queries to complete.
     */
    pthread_mutex_unlock(&(self->lock));
    Py_END_ALLOW_THREADS;
    /* the next operation the client will do is send a query, so ask him to
       wait for a writable condition */
    self->status = CONN_STATUS_SEND_DATESTYLE;
    Dprintf("conn_poll: connection is built, retrning %d",
            PSYCO_POLL_WRITE);
    return PyInt_FromLong(PSYCO_POLL_WRITE);
}

static PyObject *
psyco_conn_poll(connectionObject *self)
{
    EXC_IF_CONN_CLOSED(self);

    if (self->async) {
        return psyco_conn_poll_async(self);
    } else {
        return conn_poll_green(self);
    }
}


/* extension: fileno - return the file descriptor of the connection */

#define psyco_conn_fileno_doc \
"fileno() -> int -- Return file descriptor associated to database connection."

static PyObject *
psyco_conn_fileno(connectionObject *self)
{
    long int socket;

    EXC_IF_CONN_CLOSED(self);

    socket = (long int)PQsocket(self->pgconn);

    return PyInt_FromLong(socket);
}


/* extension: isexecuting - check for asynchronous operations */

#define psyco_conn_isexecuting_doc                           \
"isexecuting() -> bool -- Return True if the connection is " \
 "executing an asynchronous operation."

static PyObject *
psyco_conn_isexecuting(connectionObject *self)
{
    /* synchronous connections will always return False */
    if (self->async == 0) {
        Py_INCREF(Py_False);
        return Py_False;
    }

    /* check if the connection is still being built */
    if (self->status != CONN_STATUS_READY) {
        Py_INCREF(Py_True);
        return Py_True;
    }

    /* check if there is a query being executed */
    if (self->async_cursor != NULL) {
        Py_INCREF(Py_True);
        return Py_True;
    }

    /* otherwise it's not executing */
    Py_INCREF(Py_False);
    return Py_False;
}

#endif  /* PSYCOPG_EXTENSIONS */


/** the connection object **/


/* object method list */

static struct PyMethodDef connectionObject_methods[] = {
    {"cursor", (PyCFunction)psyco_conn_cursor,
     METH_VARARGS|METH_KEYWORDS, psyco_conn_cursor_doc},
    {"close", (PyCFunction)psyco_conn_close,
     METH_VARARGS, psyco_conn_close_doc},
    {"commit", (PyCFunction)psyco_conn_commit,
     METH_VARARGS, psyco_conn_commit_doc},
    {"rollback", (PyCFunction)psyco_conn_rollback,
     METH_VARARGS, psyco_conn_rollback_doc},
#ifdef PSYCOPG_EXTENSIONS
    {"set_isolation_level", (PyCFunction)psyco_conn_set_isolation_level,
     METH_VARARGS, psyco_conn_set_isolation_level_doc},
    {"set_client_encoding", (PyCFunction)psyco_conn_set_client_encoding,
     METH_VARARGS, psyco_conn_set_client_encoding_doc},
    {"get_transaction_status", (PyCFunction)psyco_conn_get_transaction_status,
     METH_VARARGS, psyco_conn_get_transaction_status_doc},
    {"get_parameter_status", (PyCFunction)psyco_conn_get_parameter_status,
     METH_VARARGS, psyco_conn_get_parameter_status_doc},
    {"get_backend_pid", (PyCFunction)psyco_conn_get_backend_pid,
     METH_NOARGS, psyco_conn_get_backend_pid_doc},
    {"lobject", (PyCFunction)psyco_conn_lobject,
     METH_VARARGS|METH_KEYWORDS, psyco_conn_lobject_doc},
    {"reset", (PyCFunction)psyco_conn_reset,
     METH_NOARGS, psyco_conn_reset_doc},
    {"poll", (PyCFunction)psyco_conn_poll,
     METH_NOARGS, psyco_conn_lobject_doc},
    {"fileno", (PyCFunction)psyco_conn_fileno,
     METH_NOARGS, psyco_conn_fileno_doc},
    {"isexecuting", (PyCFunction)psyco_conn_isexecuting,
     METH_NOARGS, psyco_conn_isexecuting_doc},
#endif
    {NULL}
};

/* object member list */

static struct PyMemberDef connectionObject_members[] = {
#ifdef PSYCOPG_EXTENSIONS
    {"closed", T_LONG, offsetof(connectionObject, closed), RO,
        "True if the connection is closed."},
    {"isolation_level", T_LONG,
        offsetof(connectionObject, isolation_level), RO,
        "The current isolation level."},
    {"encoding", T_STRING, offsetof(connectionObject, encoding), RO,
        "The current client encoding."},
    {"notices", T_OBJECT, offsetof(connectionObject, notice_list), RO},
    {"notifies", T_OBJECT, offsetof(connectionObject, notifies), RO},
    {"dsn", T_STRING, offsetof(connectionObject, dsn), RO,
        "The current connection string."},
    {"async", T_LONG, offsetof(connectionObject, async), RO,
        "True if the connection is asynchronous."},
    {"status", T_INT,
        offsetof(connectionObject, status), RO,
        "The current transaction status."},
    {"string_types", T_OBJECT, offsetof(connectionObject, string_types), RO,
        "A set of typecasters to convert textual values."},
    {"binary_types", T_OBJECT, offsetof(connectionObject, binary_types), RO,
        "A set of typecasters to convert binary values."},
    {"protocol_version", T_INT,
        offsetof(connectionObject, protocol), RO,
        "Protocol version (2 or 3) used for this connection."},
    {"server_version", T_INT,
        offsetof(connectionObject, server_version), RO,
        "Server version."},
#endif
    {NULL}
};

#define EXCEPTION_GETTER(exc) \
    { #exc, psyco_conn_get_exception, NULL, exc ## _doc, &exc }

static struct PyGetSetDef connectionObject_getsets[] = {
    /* DBAPI-2.0 extensions (exception objects) */
    EXCEPTION_GETTER(Error),
    EXCEPTION_GETTER(Warning),
    EXCEPTION_GETTER(InterfaceError),
    EXCEPTION_GETTER(DatabaseError),
    EXCEPTION_GETTER(InternalError),
    EXCEPTION_GETTER(OperationalError),
    EXCEPTION_GETTER(ProgrammingError),
    EXCEPTION_GETTER(IntegrityError),
    EXCEPTION_GETTER(DataError),
    EXCEPTION_GETTER(NotSupportedError),
    {NULL}
};
#undef EXCEPTION_GETTER

/* initialization and finalization methods */

static int
connection_setup(connectionObject *self, const char *dsn, long int async)
{
    char *pos;
    int res;

    Dprintf("connection_setup: init connection object at %p, "
	    "async %ld, refcnt = " FORMAT_CODE_PY_SSIZE_T,
            self, async, ((PyObject *)self)->ob_refcnt
      );

    self->dsn = strdup(dsn);
    self->notice_list = PyList_New(0);
    self->notifies = PyList_New(0);
    self->closed = 0;
    self->async = async;
    self->status = CONN_STATUS_SETUP;
    self->critical = NULL;
    self->async_cursor = NULL;
    self->async_status = ASYNC_DONE;
    self->pgconn = NULL;
    self->mark = 0;
    self->string_types = PyDict_New();
    self->binary_types = PyDict_New();
    self->notice_pending = NULL;
    self->encoding = NULL;

    pthread_mutex_init(&(self->lock), NULL);

    if (conn_connect(self, async) != 0) {
        Dprintf("connection_init: FAILED");
        res = -1;
    }
    else {
        Dprintf("connection_setup: good connection object at %p, refcnt = "
            FORMAT_CODE_PY_SSIZE_T,
            self, ((PyObject *)self)->ob_refcnt
          );
        res = 0;
    }

    /* here we obfuscate the password even if there was a connection error */
    pos = strstr(self->dsn, "password");
    if (pos != NULL) {
        for (pos = pos+9 ; *pos != '\0' && *pos != ' '; pos++)
            *pos = 'x';
    }

    return res;
}

static void
connection_dealloc(PyObject* obj)
{
    connectionObject *self = (connectionObject *)obj;
    
    PyObject_GC_UnTrack(self);

    if (self->closed == 0) conn_close(self);
    
    conn_notice_clean(self);

    if (self->dsn) free(self->dsn);
    if (self->encoding) free(self->encoding);
    if (self->critical) free(self->critical);

    Py_CLEAR(self->async_cursor);
    Py_CLEAR(self->notice_list);
    Py_CLEAR(self->notice_filter);
    Py_CLEAR(self->notifies);
    Py_CLEAR(self->string_types);
    Py_CLEAR(self->binary_types);

    pthread_mutex_destroy(&(self->lock));

    Dprintf("connection_dealloc: deleted connection object at %p, refcnt = "
        FORMAT_CODE_PY_SSIZE_T,
        obj, obj->ob_refcnt
      );

    obj->ob_type->tp_free(obj);
}

static int
connection_init(PyObject *obj, PyObject *args, PyObject *kwds)
{
    const char *dsn;
    long int async = 0;
    static char *kwlist[] = {"dsn", "async", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|l", kwlist, &dsn, &async))
        return -1;

    return connection_setup((connectionObject *)obj, dsn, async);
}

static PyObject *
connection_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    return type->tp_alloc(type, 0);
}

static void
connection_del(PyObject* self)
{
    PyObject_GC_Del(self);
}

static PyObject *
connection_repr(connectionObject *self)
{
    return PyString_FromFormat(
        "<connection object at %p; dsn: '%s', closed: %ld>",
        self, self->dsn, self->closed);
}

static int
connection_traverse(connectionObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->async_cursor);
    Py_VISIT(self->notice_list);
    Py_VISIT(self->notice_filter);
    Py_VISIT(self->notifies);
    Py_VISIT(self->string_types);
    Py_VISIT(self->binary_types);
    return 0;
}


/* object type */

#define connectionType_doc \
"connection(dsn, ...) -> new connection object\n\n" \
":Groups:\n" \
"  * `DBAPI-2.0 errors`: Error, Warning, InterfaceError,\n" \
"    DatabaseError, InternalError, OperationalError,\n" \
"    ProgrammingError, IntegrityError, DataError, NotSupportedError"

PyTypeObject connectionType = {
    PyObject_HEAD_INIT(NULL)
    0,
    "psycopg2._psycopg.connection",
    sizeof(connectionObject),
    0,
    connection_dealloc, /*tp_dealloc*/
    0,          /*tp_print*/
    0,          /*tp_getattr*/
    0,          /*tp_setattr*/
    0,          /*tp_compare*/
    (reprfunc)connection_repr, /*tp_repr*/
    0,          /*tp_as_number*/
    0,          /*tp_as_sequence*/
    0,          /*tp_as_mapping*/
    0,          /*tp_hash */

    0,          /*tp_call*/
    (reprfunc)connection_repr, /*tp_str*/
    0,          /*tp_getattro*/
    0,          /*tp_setattro*/
    0,          /*tp_as_buffer*/

    Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE|Py_TPFLAGS_HAVE_GC, /*tp_flags*/
    connectionType_doc, /*tp_doc*/

    (traverseproc)connection_traverse, /*tp_traverse*/
    0,          /*tp_clear*/

    0,          /*tp_richcompare*/
    0,          /*tp_weaklistoffset*/

    0,          /*tp_iter*/
    0,          /*tp_iternext*/

    /* Attribute descriptor and subclassing stuff */

    connectionObject_methods, /*tp_methods*/
    connectionObject_members, /*tp_members*/
    connectionObject_getsets, /*tp_getset*/
    0,          /*tp_base*/
    0,          /*tp_dict*/

    0,          /*tp_descr_get*/
    0,          /*tp_descr_set*/
    0,          /*tp_dictoffset*/

    connection_init, /*tp_init*/
    0, /*tp_alloc  will be set to PyType_GenericAlloc in module init*/
    connection_new, /*tp_new*/
    (freefunc)connection_del, /*tp_free  Low-level free-memory routine */
    0,          /*tp_is_gc For PyObject_IS_GC */
    0,          /*tp_bases*/
    0,          /*tp_mro method resolution order */
    0,          /*tp_cache*/
    0,          /*tp_subclasses*/
    0           /*tp_weaklist*/
};
