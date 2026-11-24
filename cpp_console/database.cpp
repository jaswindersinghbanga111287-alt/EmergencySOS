#include <mysql/jdbc.h>

sql::Connection* connectDatabase()
{
    sql::mysql::MySQL_Driver* driver =
        sql::mysql::get_mysql_driver_instance();

    sql::Connection* con =
        driver->connect(
            "tcp://127.0.0.1:3306",
            "root",
            "1"
        );

    con->setSchema("emergency_sos");

    return con;
}