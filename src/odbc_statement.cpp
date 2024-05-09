/*
  Copyright (c) 2019, 2021 IBM
  Copyright (c) 2013, Dan VerWeire<dverweire@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <napi.h>
#include <time.h>
#include <string>

#include "odbc.h"
#include "odbc_connection.h"
#include "odbc_statement.h"
#include "odbc_cursor.h"

#define DEBUG(object, f_, ...)   \
  if (object->isDebug)           \
  {                              \
    printf((f_), ##__VA_ARGS__); \
  }

#define CHECK(condition, errorCode, errorMessage, env)  \
  if ((condition))                                      \
  {                                                     \
    throwCustomMsg((errorCode), (errorMessage), (env)); \
    return;                                             \
  }

const SQLTCHAR NO_STATE_TEXT = '\0';
const size_t NO_MSG_TEXT_LENGTH = strlen(NO_MSG_TEXT);
const size_t NO_MSG_TEXT_SIZE = NO_MSG_TEXT_LENGTH * sizeof(SQLTCHAR);
// error strings

// static void throwCustomMsg(int code, const char *msg, Napi::Env env)
// {
//   SQLCHAR errMsg[SQL_MAX_MESSAGE_LENGTH + SQL_SQLSTATE_SIZE + 10];
//   sprintf((char *)errMsg, "SQLSTATE=PAERR SQLCODE=%d %s", code, msg);
//   Napi::Error::New(env, Napi::String::New(env, errMsg)).ThrowAsJavaScriptException();
// }

Napi::FunctionReference ODBCStatement::constructor;

Napi::Object ODBCStatement::Init(Napi::Env env, Napi::Object exports)
{

  Napi::HandleScope scope(env);

  Napi::Function constructorFunction = DefineClass(env, "ODBCStatement", {
                                                                             InstanceMethod("prepare", &ODBCStatement::Prepare),
                                                                             InstanceMethod("bind", &ODBCStatement::Bind),
                                                                             InstanceMethod("execute", &ODBCStatement::Execute),
                                                                             InstanceMethod("close", &ODBCStatement::Close),
                                                                         });

  // Attach the Database Constructor to the target object
  constructor = Napi::Persistent(constructorFunction);
  constructor.SuppressDestruct();

  return exports;
}

ODBCStatement::ODBCStatement(const Napi::CallbackInfo &info) : Napi::ObjectWrap<ODBCStatement>(info)
{
  this->data = new StatementData();
  this->odbcConnection = info[0].As<Napi::External<ODBCConnection>>().Data();
  this->data->hstmt = *(info[1].As<Napi::External<SQLHSTMT>>().Data());
  this->data->fetch_array = this->odbcConnection->connectionOptions.fetchArray;
  this->data->maxColumnNameLength = this->odbcConnection->getInfoResults.max_column_name_length;
  this->data->get_data_supports = this->odbcConnection->getInfoResults.sql_get_data_supports;
}

ODBCStatement::~ODBCStatement()
{
  this->Free();
}

ODBCError* ODBCStatement::GetODBCErrors(
    SQLSMALLINT handleType,
    SQLHANDLE handle)
{
  SQLRETURN return_code;
  SQLSMALLINT error_message_length = ERROR_MESSAGE_BUFFER_CHARS;
  SQLINTEGER statusRecCount;

  return_code = SQLGetDiagField(
      handleType,      // HandleType
      handle,          // Handle
      0,               // RecNumber
      SQL_DIAG_NUMBER, // DiagIdentifier
      &statusRecCount, // DiagInfoPtr
      SQL_IS_INTEGER,  // BufferLength
      NULL             // StringLengthPtr
  );

  if (!SQL_SUCCEEDED(return_code))
  {
    ODBCError *odbcErrors = new ODBCError[1];
    ODBCError error;
    error.state[0] = NO_STATE_TEXT;
    error.code = 0;
    error.message = new SQLTCHAR[NO_MSG_TEXT_LENGTH + 1];
    memcpy(error.message, NO_MSG_TEXT, NO_MSG_TEXT_SIZE + sizeof(SQLTCHAR));
    odbcErrors[0] = error;
    return odbcErrors;
  }

  ODBCError *odbcErrors = new ODBCError[statusRecCount];
  this->errorCount = statusRecCount;

  for (SQLSMALLINT i = 0; i < statusRecCount; i++)
  {

    ODBCError error;

    SQLSMALLINT new_error_message_length;
    return_code = SQL_SUCCESS;

    while (SQL_SUCCEEDED(return_code))
    {
      error.message = new SQLTCHAR[error_message_length];

      return_code =
          SQLGetDiagRec(
              handleType,               // HandleType
              handle,                   // Handle
              i + 1,                    // RecNumber
              error.state,              // SQLState
              &error.code,              // NativeErrorPtr
              error.message,            // MessageText
              error_message_length,     // BufferLength
              &new_error_message_length // TextLengthPtr
          );

      if (error_message_length > new_error_message_length)
      {
        break;
      }

      delete[] error.message;
      error_message_length = new_error_message_length + 1;
    }

    if (!SQL_SUCCEEDED(return_code))
    {
      error.state[0] = NO_STATE_TEXT;
      error.code = 0;
      memcpy(error.message, NO_MSG_TEXT, NO_MSG_TEXT_SIZE + 1);
    }

    odbcErrors[i] = error;
  }

  return odbcErrors;
}
// bool ODBCStatement::CheckAndHandleErrors(SQLRETURN return_code, SQLSMALLINT handleType, SQLHANDLE handle, const char *message)
// {
//   if (!SQL_SUCCEEDED(return_code))
//   {
//     this->errors = GetODBCErrors(handleType, handle);
//     OnError(message);
//     return true;
//   }
//   return false;
// }

void ODBCStatement::OnError(const Napi::Error &e, Napi::Function cb)
{
  Napi::Env env = Env();
  Napi::HandleScope scope(env);

  // add the additional information to the Error object
  Napi::Error error = Napi::Error::New(env, e.Message());
  Napi::Array odbcErrors = Napi::Array::New(env);

  for (SQLINTEGER i = 0; i < errorCount; i++)
  {
    ODBCError odbcError = errors[i];
    Napi::Object errorObject = Napi::Object::New(env);

    errorObject.Set(
        Napi::String::New(env, STATE),
#ifdef UNICODE
        Napi::String::New(env, (odbcError.state != NULL) ? (const char16_t *)odbcError.state : (const char16_t *)L"")
#else
        Napi::String::New(env, (odbcError.state != NULL) ? (const char *)odbcError.state : "")
#endif
    );

    errorObject.Set(
        Napi::String::New(env, CODE),
        Napi::Number::New(env, odbcError.code));

    errorObject.Set(
        Napi::String::New(env, MESSAGE),
#ifdef UNICODE
        Napi::String::New(env, (odbcError.message != NULL) ? (const char16_t *)odbcError.message : (const char16_t *)NO_MSG_TEXT)
#else
        Napi::String::New(env, (odbcError.message != NULL) ? (const char *)odbcError.message : (const char *)NO_MSG_TEXT)
#endif
    );

    // Error message has been copied off of the C ODBC error stucture, and can
    // now be deleted
    if (odbcError.message != NULL)
    {
      delete[] odbcError.message;
      odbcError.message = NULL;
    }

    odbcErrors.Set(i, errorObject);
  }

  error.Set(
      Napi::String::New(env, ODBC_ERRORS),
      odbcErrors);

  std::vector<napi_value> callbackArguments;
  callbackArguments.push_back(error.Value());

  cb.Call(callbackArguments);
}

SQLRETURN ODBCStatement::Free()
{

  SQLRETURN return_code = SQL_SUCCESS;

  if (this->data)
  {
    if (
        this->data->hstmt &&
        this->data->hstmt != SQL_NULL_HANDLE)
    {
      uv_mutex_lock(&ODBC::g_odbcMutex);
      return_code =
          SQLFreeHandle(
              SQL_HANDLE_STMT,
              this->data->hstmt);
      this->data->hstmt = SQL_NULL_HANDLE;
      uv_mutex_unlock(&ODBC::g_odbcMutex);
    }

    delete this->data;
    this->data = NULL;
  }

  return return_code;
}

/******************************************************************************
 ********************************* PREPARE ************************************
 *****************************************************************************/

// PrepareAsyncWorker, used by Prepare function (see below)
class PrepareAsyncWorker : public ODBCAsyncWorker
{

private:
  ODBCStatement *odbcStatement;
  ODBCConnection *odbcConnection;
  StatementData *data;

public:
  PrepareAsyncWorker(ODBCStatement *odbcStatement, Napi::Function &callback) : ODBCAsyncWorker(callback),
                                                                               odbcStatement(odbcStatement),
                                                                               odbcConnection(odbcStatement->odbcConnection),
                                                                               data(odbcStatement->data) {}

  ~PrepareAsyncWorker() {}

  void Execute()
  {

    SQLRETURN return_code;

    return_code = SQLPrepare(
        data->hstmt, // StatementHandle
        data->sql,   // StatementText
        SQL_NTS      // TextLength
    );
    if (!SQL_SUCCEEDED(return_code))
    {
      this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
      SetError("[odbc] Error preparing the statement\0");
      return;
    }

    // front-load the work of SQLNumParams here, so we can convert
    // NAPI/JavaScript values to C values immediately in Bind
    return_code = SQLNumParams(
        data->hstmt,          // StatementHandle
        &data->parameterCount // ParameterCountPtr
    );
    if (!SQL_SUCCEEDED(return_code))
    {
      this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
      SetError("[odbc] Error retrieving number of parameter markers to be bound to the statement\0");
      return;
    }

    data->parameters = new Parameter *[data->parameterCount];
    for (SQLSMALLINT i = 0; i < data->parameterCount; i++)
    {
      data->parameters[i] = new Parameter();
    }
  }

  void OnOK()
  {

    Napi::Env env = Env();
    Napi::HandleScope scope(env);

    std::vector<napi_value> callbackArguments;
    callbackArguments.push_back(env.Null());
    Callback().Call(callbackArguments);
  }
};

/*
 *  ODBCStatement:Prepare (Async)
 *    Description: Prepares an SQL string so that it can be bound with
 *                 parameters and then executed.
 *
 *    Parameters:
 *      const Napi::CallbackInfo& info:
 *        The information passed by Napi from the JavaScript call, including
 *        arguments from the JavaScript function. In JavaScript, the
 *        prepare() function takes two arguments.
 *
 *        info[0]: String: the SQL string to prepare.
 *        info[1]: Function: callback function:
 *            function(error, result)
 *              error: An error object if there was a problem getting results,
 *                     or null if operation was successful.
 *              result: The number of rows affected by the executed query.
 *
 *    Return:
 *      Napi::Value:
 *        Undefined (results returned in callback).
 */
Napi::Value ODBCStatement::Prepare(const Napi::CallbackInfo &info)
{

  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  this->napiParameters = Napi::Persistent(Napi::Array::New(env));

  if (!info[0].IsString() || !info[1].IsFunction())
  {
    Napi::TypeError::New(env, "Argument 0 must be a string , Argument 1 must be a function.").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::String sql = info[0].ToString();
  Napi::Function callback = info[1].As<Napi::Function>();

  if (this->data->hstmt == SQL_NULL_HANDLE)
  {
    Napi::Error error = Napi::Error::New(env, "Statment handle is no longer valid. Cannot prepare SQL on an invalid statment handle.");
    std::vector<napi_value> callbackArguments;
    callbackArguments.push_back(error.Value());
    callback.Call(callbackArguments);
    return env.Undefined();
  }

  data->sql = ODBC::NapiStringToSQLTCHAR(sql);

  PrepareAsyncWorker *worker = new PrepareAsyncWorker(this, callback);
  worker->Queue();

  return env.Undefined();
}

/******************************************************************************
 *********************************** BIND *************************************
 *****************************************************************************/

// BindAsyncWorker, used by Bind function (see below)
class BindAsyncWorker : public ODBCAsyncWorker
{

public:
  BindAsyncWorker(ODBCStatement *odbcStatement, Napi::Function &callback) : ODBCAsyncWorker(callback),
                                                                            odbcStatement(odbcStatement),
                                                                            odbcConnection(odbcStatement->odbcConnection),
                                                                            data(odbcStatement->data) {}

private:
  ODBCStatement *odbcStatement;
  ODBCConnection *odbcConnection;
  StatementData *data;

  ~BindAsyncWorker() {}

  void Execute()
  {

    SQLRETURN return_code;

    return_code = ODBC::DescribeParameters(data->hstmt, data->parameters, data->parameterCount);
    if (!SQL_SUCCEEDED(return_code))
    {
      this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
      SetError("[odbc] Error retrieving information about the parameters in the statement\0");
      return;
    }

    return_code = ODBC::BindParameters(data->hstmt, data->parameters, data->parameterCount);
    if (!SQL_SUCCEEDED(return_code))
    {
      this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
      SetError("[odbc] Error binding parameters to the statement\0");
      return;
    }
  }

  void OnOK()
  {

    Napi::Env env = Env();
    Napi::HandleScope scope(env);

    std::vector<napi_value> callbackArguments;
    callbackArguments.push_back(env.Null());
    Callback().Call(callbackArguments);
  }
};

Napi::Value ODBCStatement::Bind(const Napi::CallbackInfo &info)
{

  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  if (!info[0].IsArray() || !info[1].IsFunction())
  {
    Napi::TypeError::New(env, "Function signature is: bind(array, function)").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Array napiArray = info[0].As<Napi::Array>();
  this->napiParameters = Napi::Persistent(napiArray);
  Napi::Function callback = info[1].As<Napi::Function>();

  if (this->data->hstmt == SQL_NULL_HANDLE)
  {
    Napi::Error error = Napi::Error::New(env, "Statment handle is no longer valid. Cannot bind SQL on an invalid statment handle.");
    std::vector<napi_value> callbackArguments;
    callbackArguments.push_back(error.Value());
    callback.Call(callbackArguments);
    return env.Undefined();
  }

  // if the parameter count isnt right, end right away
  if (data->parameterCount != (SQLSMALLINT)this->napiParameters.Value().Length() || data->parameters == NULL)
  {
    std::vector<napi_value> callbackArguments;

    Napi::Error error = Napi::Error::New(env, Napi::String::New(env, "[node-odbc] Error in Statement::BindAsyncWorker::Bind: The number of parameters in the prepared statement (" + std::to_string(data->parameterCount) + ") doesn't match the number of parameters passed to bind (" + std::to_string((SQLSMALLINT)this->napiParameters.Value().Length()) + "}."));
    callbackArguments.push_back(error.Value());

    callback.Call(callbackArguments);
    return env.Undefined();
  }

  // converts NAPI/JavaScript values to values used by SQLBindParameter
  ODBC::StoreBindValues(&napiArray, this->data->parameters);

  BindAsyncWorker *worker = new BindAsyncWorker(this, callback);
  worker->Queue();

  return env.Undefined();
}

/******************************************************************************
 ********************************* EXECUTE ************************************
 *****************************************************************************/

// ExecuteAsyncWorker, used by Execute function (see below)
class ExecuteAsyncWorker : public ODBCAsyncWorker
{

private:
  ODBCConnection *odbcConnection;
  ODBCStatement *odbcStatement;
  StatementData *data;

  void Execute()
  {

    SQLRETURN return_code;

    // set SQL_ATTR_QUERY_TIMEOUT
    if (data->query_options.timeout > 0)
    {
      return_code =
          SQLSetStmtAttr(
              data->hstmt,
              SQL_ATTR_QUERY_TIMEOUT,
              (SQLPOINTER)data->query_options.timeout,
              IGNORED_PARAMETER);

      // It is possible that SQLSetStmtAttr returns a warning with SQLSTATE
      // 01S02, indicating that the driver changed the value specified.
      // Although we never use the timeout variable again (and so we don't
      // REALLY need it to be correct in the code), its just good to have
      // the correct value if we need it.
      if (return_code == SQL_SUCCESS_WITH_INFO)
      {
        return_code =
            SQLGetStmtAttr(
                data->hstmt,
                SQL_ATTR_QUERY_TIMEOUT,
                (SQLPOINTER)&data->query_options.timeout,
                SQL_IS_UINTEGER,
                IGNORED_PARAMETER);
      }

      // Both of the SQL_ATTR_QUERY_TIMEOUT calls are combined here
      if (!SQL_SUCCEEDED(return_code))
      {
        this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
        SetError("[odbc] Error setting the query timeout on the statement\0");
        return;
      }
    }

    return_code =
        set_fetch_size(
            data,
            data->query_options.fetch_size);
    if (!SQL_SUCCEEDED(return_code))
    {
      this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
      SetError("[odbc] Error setting the fetch size\0");
      return;
    }

    return_code =
        SQLExecute(
            data->hstmt // StatementHandle
        );
    if (!SQL_SUCCEEDED(return_code))
    {
      this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
      SetError("[odbc] Error executing the statement\0");
      return;
    }

    if (return_code != SQL_NO_DATA)
    {

      if (data->query_options.use_cursor)
      {
        if (data->query_options.cursor_name != NULL)
        {
          return_code =
              SQLSetCursorName(
                  data->hstmt,
                  data->query_options.cursor_name,
                  data->query_options.cursor_name_length);

          if (!SQL_SUCCEEDED(return_code))
          {
            this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
            SetError("[odbc] Error setting the cursor name on the statement\0");
            return;
          }
        }
      }

      // set_fetch_size will swallow errors in the case that the driver
      // doesn't implement SQL_ATTR_ROW_ARRAY_SIZE for SQLSetStmtAttr and
      // the fetch size was 1. If the fetch size was set by the user to a
      // value greater than 1, throw an error.
      if (!SQL_SUCCEEDED(return_code))
      {
        this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
        SetError("[odbc] Error setting the fetch size on the statement\0");
        return;
      }

      return_code =
          prepare_for_fetch(
              data);
      if (!SQL_SUCCEEDED(return_code))
      {
        this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
        SetError("[odbc] Error preparing for fetch\0");
        return;
      }

      if (!data->query_options.use_cursor)
      {
        bool alloc_error = false;
        return_code =
            fetch_all_and_store(
                data,
                true,
                &alloc_error);
        if (alloc_error)
        {
          SetError("[odbc] Error allocating or reallocating memory when fetching data. No ODBC error information available.\0");
          return;
        }
        if (!SQL_SUCCEEDED(return_code))
        {
          this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
          SetError("[odbc] Error retrieving the result set from the statement\0");
          return;
        }
      }
    }
  }

  void OnOK()
  {

    Napi::Env env = Env();
    Napi::HandleScope scope(env);

    std::vector<napi_value> callbackArguments;

    if (data->query_options.use_cursor)
    {
      // arguments for the ODBCCursor constructor
      std::vector<napi_value> cursor_arguments =
          {
              Napi::External<StatementData>::New(env, data),
              Napi::External<ODBCConnection>::New(env, this->odbcConnection),
              this->odbcStatement->napiParameters.Value(),
              Napi::Boolean::New(env, false)};

      // create a new ODBCCursor object as a Napi::Value
      Napi::Value cursorObject = ODBCCursor::constructor.New(cursor_arguments);

      // return cursor
      std::vector<napi_value> callbackArguments =
          {
              env.Null(),
              cursorObject};

      Callback().Call(callbackArguments);
    }
    else
    {
      Napi::Array rows = process_data_for_napi(env, data, odbcStatement->napiParameters.Value());

      std::vector<napi_value> callbackArguments;
      callbackArguments.push_back(env.Null());
      callbackArguments.push_back(rows);

      Callback().Call(callbackArguments);
    }

    return;
  }

public:
  ExecuteAsyncWorker(ODBCStatement *odbcStatement, Napi::Function &callback) : ODBCAsyncWorker(callback),
                                                                               odbcConnection(odbcStatement->odbcConnection),
                                                                               odbcStatement(odbcStatement),
                                                                               data(odbcStatement->data) {}

  ~ExecuteAsyncWorker() {}
};

Napi::Value ODBCStatement::Execute(const Napi::CallbackInfo &info)
{

  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Napi::Function callback;

  size_t argument_count = info.Length();
  // ensuring the passed parameters are correct
  if ((argument_count == 1 && info[0].IsFunction()) || (argument_count == 2 && info[1].IsFunction()))
  {
    callback = info[argument_count - 1].As<Napi::Function>();
  }

  Napi::Value error;

  // ensuring the passed parameters are correct
  if (argument_count >= 1 && info[0].IsObject())
  {
    error =
        parse_query_options(
            env,
            info[0].As<Napi::Object>(),
            &this->data->query_options);
  }
  else
  {
    error =
        parse_query_options(
            env,
            env.Null(),
            &this->data->query_options);
  }

  if (!error.IsNull())
  {
    // Error when parsing the query options. Return the callback with the error
    std::vector<napi_value> callback_argument =
        {
            error};
    callback.Call(callback_argument);
  }

  if (this->data->hstmt == SQL_NULL_HANDLE)
  {
    Napi::Error error = Napi::Error::New(env, "Statment handle is no longer valid. Cannot execute SQL on an invalid statment handle.");
    std::vector<napi_value> callbackArguments;
    callbackArguments.push_back(error.Value());
    callback.Call(callbackArguments);
    return env.Undefined();
  }

  ExecuteAsyncWorker *worker = new ExecuteAsyncWorker(this, callback);
  worker->Queue();

  return env.Undefined();
}

/******************************************************************************
 ********************************** CLOSE *************************************
 *****************************************************************************/

// CloseStatementAsyncWorker, used by Close function (see below)
class CloseStatementAsyncWorker : public ODBCAsyncWorker
{

private:
  ODBCStatement *odbcStatement;
  StatementData *data;

  void Execute()
  {

    SQLRETURN return_code;

    return_code = odbcStatement->Free();
    if (!SQL_SUCCEEDED(return_code))
    {
      this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
      SetError("[odbc] Error closing the Statement\0");
      return;
    }
  }

  void OnOK()
  {

    Napi::Env env = Env();
    Napi::HandleScope scope(env);

    std::vector<napi_value> callbackArguments;
    callbackArguments.push_back(env.Null());
    Callback().Call(callbackArguments);
  }

public:
  CloseStatementAsyncWorker(ODBCStatement *odbcStatement, Napi::Function &callback) : ODBCAsyncWorker(callback),
                                                                                      odbcStatement(odbcStatement),
                                                                                      data(odbcStatement->data) {}

  ~CloseStatementAsyncWorker() {}
};

Napi::Value ODBCStatement::Close(const Napi::CallbackInfo &info)
{

  Napi::Env env = info.Env();
  Napi::HandleScope scope(env);

  Napi::Function callback = info[0].As<Napi::Function>();

  CloseStatementAsyncWorker *worker = new CloseStatementAsyncWorker(this, callback);
  worker->Queue();

  return env.Undefined();
}

// Napi::Value ODBCStatement::ExecuteSync(ODBCStatement *odbcStatement,
//                                        ODBCConnection odbcConnection,
//                                        StatementData data, Napi::CallbackInfo &info)
// {
//   // private:
//   ODBCConnection *odbcConnection;
//   ODBCStatement *odbcStatement;
//   StatementData *data;

//   Napi::Env env = info.Env();
//   Napi::HandleScope scope(env);

//   Napi::Function callback;

//   size_t argument_count = info.Length();
//   // ensuring the passed parameters are correct
//   if ((argument_count == 1 && info[0].IsFunction()) || (argument_count == 2 && info[1].IsFunction()))
//   {
//     callback = info[argument_count - 1].As<Napi::Function>();
//   }

//   Napi::Value error;

//   // ensuring the passed parameters are correct
//   if (argument_count >= 1 && info[0].IsObject())
//   {
//     error =
//         parse_query_options(
//             env,
//             info[0].As<Napi::Object>(),
//             &this->data->query_options);
//   }
//   else
//   {
//     error =
//         parse_query_options(
//             env,
//             env.Null(),
//             &this->data->query_options);
//   }

//   if (!error.IsNull())
//   {
//     // Error when parsing the query options. Return the callback with the error
//     std::vector<napi_value> callback_argument =
//         {
//             error};
//     callback.Call(callback_argument);
//   }

//   if (this->data->hstmt == SQL_NULL_HANDLE)
//   {
//     Napi::Error error = Napi::Error::New(env, "Statment handle is no longer valid. Cannot execute SQL on an invalid statment handle.");
//     std::vector<napi_value> callbackArguments;
//     callbackArguments.push_back(error.Value());
//     callback.Call(callbackArguments);
//     return env.Undefined();
//   }

//   SQLRETURN return_code;

//   // set SQL_ATTR_QUERY_TIMEOUT
//   if (data->query_options.timeout > 0)
//   {
//     return_code =
//         SQLSetStmtAttr(
//             data->hstmt,
//             SQL_ATTR_QUERY_TIMEOUT,
//             (SQLPOINTER)data->query_options.timeout,
//             IGNORED_PARAMETER);

//     // It is possible that SQLSetStmtAttr returns a warning with SQLSTATE
//     // 01S02, indicating that the driver changed the value specified.
//     // Although we never use the timeout variable again (and so we don't
//     // REALLY need it to be correct in the code), its just good to have
//     // the correct value if we need it.
//     if (return_code == SQL_SUCCESS_WITH_INFO)
//     {
//       return_code =
//           SQLGetStmtAttr(
//               data->hstmt,
//               SQL_ATTR_QUERY_TIMEOUT,
//               (SQLPOINTER)&data->query_options.timeout,
//               SQL_IS_UINTEGER,
//               IGNORED_PARAMETER);
//     }

//     // Both of the SQL_ATTR_QUERY_TIMEOUT calls are combined here
//     //   if (!SQL_SUCCEEDED(return_code)) {
//     //     this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
//     //     SetError("[odbc] Error setting the query timeout on the statement\0");
//     //     return;
//     //   }
//     // }

//     return_code =
//         set_fetch_size(
//             data,
//             data->query_options.fetch_size);
//     // if (!SQL_SUCCEEDED(return_code)) {
//     //   this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
//     //   SetError("[odbc] Error setting the fetch size\0");
//     //   return;
//     // }

//     return_code =
//         SQLExecute(
//             data->hstmt // StatementHandle
//         );
//     // if (!SQL_SUCCEEDED(return_code)) {
//     //   this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
//     //   SetError("[odbc] Error executing the statement\0");
//     //   return;
//     // }

//     if (return_code != SQL_NO_DATA)
//     {
//       if (data->query_options.use_cursor)
//       {
//         if (data->query_options.cursor_name != NULL)
//         {
//           return_code =
//               SQLSetCursorName(
//                   data->hstmt,
//                   data->query_options.cursor_name,
//                   data->query_options.cursor_name_length);

//           // if (!SQL_SUCCEEDED(return_code)) {
//           //   this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
//           //   SetError("[odbc] Error setting the cursor name on the statement\0");
//           //   return;
//           // }
//         }
//       }

//       // set_fetch_size will swallow errors in the case that the driver
//       // doesn't implement SQL_ATTR_ROW_ARRAY_SIZE for SQLSetStmtAttr and
//       // the fetch size was 1. If the fetch size was set by the user to a
//       // value greater than 1, throw an error.
//       // if (!SQL_SUCCEEDED(return_code)) {
//       //   this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
//       //   SetError("[odbc] Error setting the fetch size on the statement\0");
//       //   return;
//       // }

//       return_code =
//           prepare_for_fetch(
//               data);
//       // if (!SQL_SUCCEEDED(return_code)) {
//       //   this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
//       //   SetError("[odbc] Error preparing for fetch\0");
//       //   return;
//       // }

//       if (!data->query_options.use_cursor)
//       {
//         bool alloc_error = false;
//         return_code =
//             fetch_all_and_store(
//                 data,
//                 true,
//                 &alloc_error);
//         if (alloc_error)
//         {
//           Napi::Error error = Napi::Error::New(env, "[odbc] Error allocating or reallocating memory when fetching data. No ODBC error information available.\0");
//           error.ThrowAsJavaScriptException();
//           return env.Undefined();
//           // SetError("[odbc] Error allocating or reallocating memory when fetching data. No ODBC error information available.\0");
//         }
//         // if (!SQL_SUCCEEDED(return_code)) {
//         //   this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
//         //   SetError("[odbc] Error retrieving the result set from the statement\0");
//         //   return;
//         // }
//       }

//       // std::vector<napi_value> callbackArguments;
//       // Napi::Function cb;
//       // if (data->query_options.use_cursor)
//       // {
//       //   // arguments for the ODBCCursor constructor
//       //   std::vector<napi_value> cursor_arguments =
//       //   {
//       //     Napi::External<StatementData>::New(env, data),
//       //     Napi::External<ODBCConnection>::New(env, this->odbcConnection),
//       //     this->odbcStatement->napiParameters.Value(),
//       //     Napi::Boolean::New(env, false)
//       //   };

//       //   // create a new ODBCCursor object as a Napi::Value
//       //   Napi::Value cursorObject = ODBCCursor::constructor.New(cursor_arguments);

//       //   // return cursor
//       //   std::vector<napi_value> callbackArguments =
//       //   {
//       //     env.Null(),
//       //     cursorObject
//       //   };

//       //   // Callback().Call(callbackArguments);
//       // }
//       // else
//       // {
//       Napi::Array rows = process_data_for_napi(env, data, odbcStatement->napiParameters.Value());

//       std::vector<napi_value> callbackArguments;
//       callbackArguments.push_back(env.Null());
//       callbackArguments.push_back(rows);
//       return rows;
//       // Callback().Call(callbackArguments);
//       // }
//       // return;

//       // public:
//       //   ExecuteAsyncWorker(ODBCStatement *odbcStatement, Napi::Function& callback) : ODBCAsyncWorker(callback),
//       //     odbcConnection(odbcStatement->odbcConnection),
//       //     odbcStatement(odbcStatement),
//       //     data(odbcStatement->data) {}

//       //   ~ExecuteAsyncWorker() {}
//     }
//     else
//     {
//       return env.Undefined();
//     }
//   }
// }
// Called after a stmt has been prepared.
// Needs an array of arrays representing multiple rows of to-be-bound data
// [ [ [value, column], [value, column] ], [ [value, column], [value, column] ] ]
Napi::Value ODBCStatement::PrepBindExecuteSync(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();
  // Piece-out variables
  Napi::HandleScope scope(env);
  // Third is callback
  Napi::Function callback = info[2].As<Napi::Function>();
  if (!info[0].IsArray() || !info[1].IsFunction())
  {
    Napi::TypeError::New(env, "Function signature is: bind(array, function)").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (this->data->hstmt == SQL_NULL_HANDLE)
  {
    Napi::Error error = Napi::Error::New(env, "Statment handle is no longer valid. Cannot bind SQL on an invalid statment handle.");
    std::vector<napi_value> callbackArguments;
    callbackArguments.push_back(error.Value());
    callback.Call(callbackArguments);
    return env.Undefined();
  }
  //
  // First param to function is the sql string
  Napi::String sql = info[0].ToString();
  // 2nd param is parameters array
  Napi::Array napiArray = info[0].As<Napi::Array>();
  // Check user input
  this->napiParameters = Napi::Persistent(napiArray);

  SQLRETURN return_code; // Stores return value of following SQL functions - gets overwritten a lot.
                         //////
                         // Keep in mind that this process implicitly modifies the `hstmt` under-the-covers
                         // For example, `SQLNumParams` below has to run *after* `SQLPrepare` does its buisness with the statement handle.
                         // Otherwise, `SQLNumParams` has no clue how many parameters are in your query.
                         /////
  return_code = SQLPrepare(
      data->hstmt, // StatementHandle
      data->sql,   // StatementText
      SQL_NTS      // TextLength
  );
  if (!SQL_SUCCEEDED(return_code))
  {
    this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
    Napi::Error error = Napi::Error::New(env, "Statment handle is no longer valid. Cannot bind SQL on an invalid statment handle.");
    OnError(error, callback);
    return env.Undefined();
  }

  // front-load the work of SQLNumParams here, so we can convert
  // NAPI/JavaScript values to C values immediately in Bind
  // SQLNumParams puts your answer into &data->parameterCount
  return_code = SQLNumParams(
      data->hstmt,          // StatementHandle
      &data->parameterCount // ParameterCountPtr
  );
  if (!SQL_SUCCEEDED(return_code))
  {
    this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
    Napi::Error error = Napi::Error::New(env, "Statment handle is no longer valid. Cannot bind SQL on an invalid statment handle.");
    OnError(error, callback);
    return env.Undefined();
  }
  /////
  // We now know how many parameters are needed for the query to run.
  // Now we put the given parameters from the caller into a bindable format.

  // if the parameter count isnt right, end right away
  // Just use the first item in the array as a quick test.

  // if (data->parameterCount != (SQLSMALLINT)this->napiParameters[0].Value().Length() || data->parameters == NULL)
  // {
  //   std::vector<napi_value> callbackArguments;

  //   Napi::Error error = Napi::Error::New(env, Napi::String::New(env, "[node-odbc] Error in Statement::BindAsyncWorker::Bind: The number of parameters in the prepared statement (" + std::to_string(data->parameterCount) + ") doesn't match the number of parameters passed to bind (" + std::to_string((SQLSMALLINT)this->napiParameters.Value().Length()) + "}."));
  //   callbackArguments.push_back(error.Value());

  //   callback.Call(callbackArguments);
  //   return env.Undefined();
  // }
  return Napi::Number::New(env,return_code);
  // array of Parameters
  // converts NAPI/JavaScript values to values used by SQLBindParameter
  // ODBC::StoreBindValues(&napiArray, this->data->parameters);
  // // Determine data type of column
  // return_code = ODBC::DescribeParameters(this->data->hstmt, this->data->parameters, this->data->parameterCount);
  // if (!SQL_SUCCEEDED(return_code))
  // {
  //   this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
  //   SetError("[odbc] Error retrieving information about the parameters in the statement\0");
  //   return;
  // }

  // return_code = ODBC::BindParameters(data->hstmt, data->parameters, data->parameterCount);
  // if (!SQL_SUCCEEDED(return_code))
  // {
  //   this->errors = GetODBCErrors(SQL_HANDLE_STMT, data->hstmt);
  //   SetError("[odbc] Error binding parameters to the statement\0");
  //   return;
  // }
  // // Pass over to our sync worker
  // Napi::Value qsyncworker = QuerySyncWorker(this, napiParameterArray, data, callback, env);
}
