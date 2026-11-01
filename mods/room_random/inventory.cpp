#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include "inventory.h"
#include "sdgo/owned_unit_record.h"

namespace room_random_inventory {
namespace {
using sdgo::owned_unit_record::Row;
struct Handles {
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC db = SQL_NULL_HDBC;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    ~Handles() {
        if (stmt) SQLFreeHandle(SQL_HANDLE_STMT,stmt);
        if (db) { SQLDisconnect(db); SQLFreeHandle(SQL_HANDLE_DBC,db); }
        if (env) SQLFreeHandle(SQL_HANDLE_ENV,env);
    }
};
void Check(SQLRETURN rc) { if (!SQL_SUCCEEDED(rc)) throw std::runtime_error("ODBC operation failed"); }
std::wstring Ini(const std::wstring& file, const wchar_t* key) {
    wchar_t text[1024]{};
    const DWORD n=GetPrivateProfileStringW(L"DB Game",key,L"",text,1024,file.c_str());
    if (!n || n==1023) throw std::runtime_error("Database INI missing a required DB Game setting");
    return text;
}
std::wstring Escape(const std::wstring& text) {
    std::wstring result=L"{";
    for (wchar_t ch:text) { result+=ch; if(ch==L'}') result+=L'}'; }
    return result+L"}";
}
int64_t Number(SQLHSTMT stmt, SQLUSMALLINT column) {
    SQLBIGINT n=0; SQLLEN bytes=0;
    Check(SQLGetData(stmt,column,SQL_C_SBIGINT,&n,sizeof(n),&bytes));
    return bytes==SQL_NULL_DATA ? 0 : n;
}
template<class T> T N(SQLHSTMT stmt, SQLUSMALLINT& column) {
    const int64_t n=Number(stmt,column++);
    if (n<0 || static_cast<uint64_t>(n)>static_cast<uint64_t>((std::numeric_limits<T>::max)()))
        throw std::runtime_error("Inventory field outside native record bounds");
    return static_cast<T>(n);
}
std::array<uint8_t,16> Date(SQLHSTMT stmt, SQLUSMALLINT& column) {
    SQL_TIMESTAMP_STRUCT t{}; SQLLEN bytes=0;
    Check(SQLGetData(stmt,column++,SQL_C_TYPE_TIMESTAMP,&t,sizeof(t),&bytes));
    std::array<uint8_t,16> data{};
    static_assert(sizeof(t)==16,"Native timestamp layout changed");
    if (bytes!=SQL_NULL_DATA) std::memcpy(data.data(),&t,16);
    return data;
}
} // namespace

bool Read(const Settings& settings, const std::vector<uint32_t>& owners,
          std::vector<sdgo::random_loadout::Candidate>& output, std::string& error) {
    output.clear(); error.clear();
    if (settings.databaseIni.empty() || GetFileAttributesW(settings.databaseIni.c_str())==INVALID_FILE_ATTRIBUTES) {
        error="Configure DatabaseIni with the host's existing WorldServer INI"; return false;
    }
    if (owners.empty() || owners.size()>12 || settings.timeoutSeconds<1 || settings.timeoutSeconds>10
        || settings.maxRowsPerPlayer<1 || settings.maxRowsPerPlayer>16384) {
        error="Invalid room inventory request bounds"; return false;
    }
    Handles h;
    const char* stage="ODBC setup";
    try {
        Check(SQLAllocHandle(SQL_HANDLE_ENV,SQL_NULL_HANDLE,&h.env));
        Check(SQLSetEnvAttr(h.env,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),0));
        Check(SQLAllocHandle(SQL_HANDLE_DBC,h.env,&h.db));
        Check(SQLSetConnectAttrW(h.db,SQL_LOGIN_TIMEOUT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(settings.timeoutSeconds)),0));
        Check(SQLSetConnectAttrW(h.db,SQL_ATTR_ACCESS_MODE,reinterpret_cast<SQLPOINTER>(SQL_MODE_READ_ONLY),0));
        std::wstring connection=L"DRIVER="+Escape(settings.driver)+L";SERVER="+Escape(Ini(settings.databaseIni,L"DBServerName"))
            +L";DATABASE="+Escape(Ini(settings.databaseIni,L"DBName"))+L";UID="+Escape(Ini(settings.databaseIni,L"ID"))
            +L";PWD="+Escape(Ini(settings.databaseIni,L"PWD"))+L";";
        stage="database connection";
        const SQLRETURN connected=SQLDriverConnectW(h.db,nullptr,reinterpret_cast<SQLWCHAR*>(connection.data()),
            SQL_NTS,nullptr,0,nullptr,SQL_DRIVER_NOPROMPT);
        SecureZeroMemory(connection.data(),connection.size()*sizeof(wchar_t));
        Check(connected);
        Check(SQLAllocHandle(SQL_HANDLE_STMT,h.db,&h.stmt));
        stage="query setup";
        Check(SQLSetStmtAttrW(h.stmt,SQL_ATTR_QUERY_TIMEOUT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(settings.timeoutSeconds)),0));
        const size_t maxRows=owners.size()*settings.maxRowsPerPlayer;
        std::wstring query=L"SELECT TOP ("+std::to_wstring(maxRows+1)+L") u.AccountNumber,u.UserUnitNumber,u.UserUnitSlotNumber,"
            L"u.UnitNumber,u.UnitLevelNumber,CASE WHEN u.StateSaleIntention='Y' THEN 1 ELSE 0 END,u.TermValidityUse,u.UnitKill,"
            L"u.UnitColorNumber_1,u.UnitColorNumber_2,u.UnitColorNumber_3,u.UnitColorNumber_4,u.UnitColorNumber_5,u.UnitColorNumber_6,"
            L"u.UnitDecalNumber_1,u.UnitDecalNumber_2,u.UnitDecalNumber_3,u.CubeMapLevel,u.CurEnergy,u.UnitBattle,u.SkillPartID,"
            L"u.DateGet,u.DateEnd,u.CategoryNumber,u.ShopUnit,u.RunOut,u.Forbidden,u.TimeLimit,u.ShopItemtblidx,"
            L"u.BattleShipCount,u.LapUnitExp,u.Normal_MaxLv,u.LockState,u.UnitSkill_1,u.UnitSkill_2,u.UnitSkill_3,u.UnitSkill_4,"
            L"u.ResetCnt,u.UnitClass,u.UnitClassMark,u.ComposeExp,u.ComposeRepair,u.ComposeSkillReset,"
            L"d.UnitGradeNumber,d.Star FROM dbo.tbl_UserUnits u JOIN dbo.tbl_Units d ON d.UnitNumber=u.UnitNumber "
            L"WHERE u.AccountNumber IN (";
        for(size_t i=0;i<owners.size();++i) query+=(i?L",?":L"?");
        query+=L") AND u.Forbidden=0 AND u.RunOut=0 AND u.CurEnergy>0 "
            L"AND d.SallyReady=1 AND d.UnitGradeNumber2<>1 "
            L"AND (u.TimeLimit<>1 OR u.DateEnd>GETDATE()) ORDER BY u.AccountNumber,u.UserUnitSlotNumber";
        Check(SQLPrepareW(h.stmt,reinterpret_cast<SQLWCHAR*>(query.data()),SQL_NTS));
        std::vector<SQLINTEGER> params; params.reserve(owners.size());
        for (uint32_t owner:owners) {
            if (owner>INT32_MAX) throw std::runtime_error("Invalid human owner ID");
            params.push_back(static_cast<SQLINTEGER>(owner));
        }
        for (size_t i=0;i<params.size();++i)
            Check(SQLBindParameter(h.stmt,static_cast<SQLUSMALLINT>(i+1),SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,
                                   0,0,&params[i],sizeof(SQLINTEGER),nullptr));
        stage="inventory query";
        Check(SQLExecute(h.stmt));
        std::map<uint32_t,unsigned> perOwner;
        stage="inventory row conversion";
        for (;;) {
            const SQLRETURN rc=SQLFetch(h.stmt);
            if (rc==SQL_NO_DATA) break;
            Check(rc);
            SQLUSMALLINT col=1;
            sdgo::random_loadout::Candidate u;
            Row r;
            u.owner=N<uint32_t>(h.stmt,col); r.serial=N<uint64_t>(h.stmt,col); r.slot=N<uint16_t>(h.stmt,col);
            r.unit=N<uint32_t>(h.stmt,col); r.level=N<uint8_t>(h.stmt,col); r.saleAllowed=N<uint8_t>(h.stmt,col);
            r.validity=N<uint16_t>(h.stmt,col); r.kills=N<uint32_t>(h.stmt,col);
            for(auto& v:r.colours) v=sdgo::owned_unit_record::WordBits(Number(h.stmt,col++));
            for(auto& v:r.decals) v=N<uint32_t>(h.stmt,col);
            r.shine=N<uint16_t>(h.stmt,col);
            SQLREAL energy=0; SQLLEN bytes=0; Check(SQLGetData(h.stmt,col++,SQL_C_FLOAT,&energy,sizeof(energy),&bytes));
            r.energy=bytes==SQL_NULL_DATA ? 0.0f : energy;
            r.battles=N<uint32_t>(h.stmt,col); r.skill=N<uint32_t>(h.stmt,col);
            r.acquired=Date(h.stmt,col); r.expires=Date(h.stmt,col);
            r.category=N<uint32_t>(h.stmt,col); r.shop=N<uint8_t>(h.stmt,col); r.runOut=N<uint8_t>(h.stmt,col);
            r.forbidden=N<uint8_t>(h.stmt,col); r.timeLimit=N<uint8_t>(h.stmt,col); r.shopIndex=N<uint32_t>(h.stmt,col);
            r.deckCount=N<uint16_t>(h.stmt,col); r.experience=N<uint32_t>(h.stmt,col); r.normalMax=N<uint8_t>(h.stmt,col);
            r.locked=N<uint8_t>(h.stmt,col);
            for(auto& v:r.customSkills) v=sdgo::owned_unit_record::WordBits(Number(h.stmt,col++));
            r.resetCount=N<uint16_t>(h.stmt,col); r.unitClass=N<uint8_t>(h.stmt,col); r.classMark=N<uint8_t>(h.stmt,col);
            r.composeExp=N<uint8_t>(h.stmt,col); r.composeRepair=N<uint8_t>(h.stmt,col); r.composeSkill=N<uint8_t>(h.stmt,col);
            u.rank=N<uint8_t>(h.stmt,col); u.points=N<uint8_t>(h.stmt,col);
            if (std::find(owners.begin(),owners.end(),u.owner)==owners.end()
                || ++perOwner[u.owner]>settings.maxRowsPerPlayer || output.size()>=maxRows)
                throw std::runtime_error("Inventory owner or row bound mismatch");
            u.unit=r.unit; u.serial=r.serial; u.data=sdgo::owned_unit_record::Pack(r);
            u.eligible=r.level>0 && r.normalMax<=4 && std::isfinite(r.energy) && r.energy>0;
            output.push_back(u);
        }
        return true;
    } catch (const std::exception& ex) {
        // Never include the connection string, password, or full driver message.
        error=std::string(stage)+": "+ex.what();
        SQLCHAR state[6]{},message[1]{}; SQLINTEGER native=0; SQLSMALLINT length=0;
        const SQLSMALLINT type=h.stmt?SQL_HANDLE_STMT:SQL_HANDLE_DBC;
        const SQLHANDLE handle=h.stmt?h.stmt:h.db;
        if(handle && SQL_SUCCEEDED(SQLGetDiagRecA(type,handle,1,state,&native,message,1,&length)))
            error+=" [SQLSTATE "+std::string(reinterpret_cast<char*>(state))+", native "+std::to_string(native)+"]";
        output.clear(); return false;
    }
}
} // namespace room_random_inventory
