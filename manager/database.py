"""Owner-selected native INI connections. Credentials never enter HTTP results."""
from pathlib import Path
from .storage import ini_values


def connection_string(ini, section='DB Game'):
    import pyodbc
    parser = ini_values(Path(ini).read_bytes())
    name = next((s for s in parser.sections() if s.replace(' ', '').casefold() == section.replace(' ', '').casefold()), None)
    if name is None: raise ValueError('所选 INI 缺少 ' + section)
    values = parser[name]
    escape = lambda v: '{' + str(v).replace('}', '}}') + '}'
    drivers = [d for d in pyodbc.drivers() if 'SQL Server' in d]
    if not drivers: raise ValueError('服主电脑尚未安装 SQL Server ODBC 驱动')
    return 'DRIVER='+escape(drivers[-1])+';SERVER='+escape(values['DBServerName'])+';DATABASE='+escape(values['DBName'])+';UID='+escape(values['ID'])+';PWD='+escape(values['PWD'])+';APP=SDGOModManager;'


def connect(ini, section='DB Game', autocommit=False):
    import pyodbc
    try:
        result = pyodbc.connect(connection_string(ini, section), timeout=5, autocommit=autocommit)
        result.timeout = 15
        return result
    except pyodbc.Error as error:
        raise ValueError('数据库连接未完成（SQLSTATE '+str(error.args[0])[:5]+'）') from None
