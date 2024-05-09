#include <napi.h>
#include <time.h>
#include <string>

#include "odbc.h"
#include "odbc_connection.h"
#include "odbc_statement.h"
#include "odbc_cursor.h"

#define DEBUG(object, f_, ...)       \
    if (object->isDebug)             \
    {                                \
        printf((f_), ##__VA_ARGS__); \
    }

#define CHECK(condition, errorCode, errorMessage, env)      \
    if ((condition))                                        \
    {                                                       \
        throwCustomMsg((errorCode), (errorMessage), (env)); \
        return;                                             \
    }
static void throwCustomMsg(int code, const char *msg, Napi::Env env)
{
    SQLCHAR errMsg[SQL_MAX_MESSAGE_LENGTH + SQL_SQLSTATE_SIZE + 10];
    sprintf((char *)errMsg, "SQLSTATE=PAERR SQLCODE=%d %s", code, msg);
    Napi::Error::New(env, Napi::String::New(env, errMsg)).ThrowAsJavaScriptException();
}

Napi::FunctionReference ODBCSyncStatement::constructor;

Napi::Object ODBCSyncStatement::Init(Napi::Env env, Napi::Object exports)
{

    Napi::HandleScope scope(env);

    Napi::Function constructorFunction = DefineClass(env, "ODBCSyncStatement", {
                                                                                   InstanceMethod("prepare", &ODBCSyncStatement::Prepare),
                                                                                   InstanceMethod("bind", &ODBCSyncStatement::Bind),
                                                                                   InstanceMethod("execute", &ODBCSyncStatement::Execute),
                                                                                   InstanceMethod("close", &ODBCSyncStatement::Close),
                                                                               });

    // Attach the Database Constructor to the target object
    constructor = Napi::Persistent(constructorFunction);
    constructor.SuppressDestruct();

    return exports;
}

ODBCSyncStatement::ODBCSyncStatement(const Napi::CallbackInfo &info) : Napi::ObjectWrap<ODBCSyncStatement>(info)
{
    this->data = new StatementData();
    this->odbcConnection = info[0].As<Napi::External<ODBCConnection>>().Data();
    this->data->hstmt = *(info[1].As<Napi::External<SQLHSTMT>>().Data());
    this->data->fetch_array = this->odbcConnection->connectionOptions.fetchArray;
    this->data->maxColumnNameLength = this->odbcConnection->getInfoResults.max_column_name_length;
    this->data->get_data_supports = this->odbcConnection->getInfoResults.sql_get_data_supports;
}

ODBCSyncStatement::~ODBCSyncStatement()
{
    this->Free();
}

SQLRETURN ODBCSyncStatement::Free()
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

Napi::Value ODBCSyncStatement::Bind(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    std::vector<napi_value> callbackArguments;

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
        callbackArguments.push_back(error.Value());
        callback.Call(callbackArguments);
        return env.Undefined();
    }

    // if the parameter count isnt right, end right away
    if (data->parameterCount != (SQLSMALLINT)this->napiParameters.Value().Length() || data->parameters == NULL)
    {
        Napi::Error error = Napi::Error::New(env, Napi::String::New(env, "[node-odbc] Error in Statement::BindAsyncWorker::Bind: The number of parameters in the prepared statement (" + std::to_string(data->parameterCount) + ") doesn't match the number of parameters passed to bind (" + std::to_string((SQLSMALLINT)this->napiParameters.Value().Length()) + "}."));
        callbackArguments.push_back(error.Value());
        callback.Call(callbackArguments);
        return env.Undefined();
    }

    // converts NAPI/JavaScript values to values used by SQLBindParameter
    ODBC::StoreBindValues(&napiArray, this->data->parameters);
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
    // BindAsyncWorker *worker = new BindAsyncWorker(this, callback);
    // worker->Queue();

    return env.Undefined();
}
