#include <iostream>
#include <string>
#include <mysql/jdbc.h>

using namespace std;


// ============================================================
// NORMAL REGISTRATION
// ============================================================

void normalRegistration(sql::Connection* con)
{
    string aadhaar;
    string name;
    int age;
    string gender;
    string contact;
    string address;

    cout << "\n===== NORMAL REGISTRATION =====\n";

    cout << "Enter Aadhaar: ";
    cin >> aadhaar;

    cin.ignore();

    cout << "Enter Name: ";
    getline(cin, name);

    cout << "Enter Age: ";
    cin >> age;

    cout << "Enter Gender: ";
    cin >> gender;

    cout << "Enter Contact: ";
    cin >> contact;

    cin.ignore();

    cout << "Enter Address: ";
    getline(cin, address);

    try
    {
        sql::PreparedStatement* pstmt =
            con->prepareStatement(
                "INSERT INTO patients "
                "(aadhaar, name, age, gender, contact, address) "
                "VALUES (?, ?, ?, ?, ?, ?)"
            );

        pstmt->setString(1, aadhaar);
        pstmt->setString(2, name);
        pstmt->setInt(3, age);
        pstmt->setString(4, gender);
        pstmt->setString(5, contact);
        pstmt->setString(6, address);

        pstmt->executeUpdate();

        delete pstmt;

        sql::Statement* stmt = con->createStatement();

        sql::ResultSet* res =
            stmt->executeQuery("SELECT LAST_INSERT_ID()");

        if (res->next())
        {
            cout << "\nPatient registered successfully!\n";
            cout << "Patient ID: "
                 << res->getInt(1) << "\n";
        }

        delete res;
        delete stmt;
    }
    catch (sql::SQLException& e)
    {
        cout << "\nRegistration failed!\n";
        cout << e.what() << "\n";
    }
}


// ============================================================
// EMERGENCY REGISTRATION
// ============================================================

void emergencyRegistration(sql::Connection* con)
{
    string aadhaar;

    cout << "\n===== EMERGENCY REGISTRATION =====\n";

    cout << "Enter Aadhaar: ";
    cin >> aadhaar;

    try
    {
        sql::PreparedStatement* pstmt =
            con->prepareStatement(
                "SELECT patient_id, name, age, gender, "
                "contact, address "
                "FROM patients "
                "WHERE aadhaar = ?"
            );

        pstmt->setString(1, aadhaar);

        sql::ResultSet* res =
            pstmt->executeQuery();

        if (res->next())
        {
            cout << "\nPatient already exists!\n";

            cout << "Patient ID: "
                 << res->getInt("patient_id") << "\n";

            cout << "Name: "
                 << res->getString("name") << "\n";

            cout << "Age: "
                 << res->getInt("age") << "\n";

            cout << "Gender: "
                 << res->getString("gender") << "\n";

            cout << "Contact: "
                 << res->getString("contact") << "\n";

            cout << "Address: "
                 << res->getString("address") << "\n";

            delete res;
            delete pstmt;
            return;
        }

        delete res;
        delete pstmt;


        // New emergency patient
        sql::PreparedStatement* insertStmt =
            con->prepareStatement(
                "INSERT INTO patients (aadhaar) "
                "VALUES (?)"
            );

        insertStmt->setString(1, aadhaar);

        insertStmt->executeUpdate();

        delete insertStmt;


        sql::Statement* stmt = con->createStatement();

        sql::ResultSet* result =
            stmt->executeQuery(
                "SELECT LAST_INSERT_ID()"
            );

        if (result->next())
        {
            cout << "\nNew emergency patient registered!\n";
            cout << "Patient ID: "
                 << result->getInt(1) << "\n";
        }

        delete result;
        delete stmt;
    }
    catch (sql::SQLException& e)
    {
        cout << "\nEmergency registration failed!\n";
        cout << e.what() << "\n";
    }
}


// ============================================================
// REGISTRATION MENU
// ============================================================

void registrationMenu(sql::Connection* con)
{
    int choice;

    while (true)
    {
        cout << "\n===== PATIENT REGISTRATION =====\n";
        cout << "1. Emergency Registration\n";
        cout << "2. Normal Registration\n";
        cout << "3. Back\n";

        cout << "Enter choice: ";
        cin >> choice;

        switch (choice)
        {
            case 1:
                emergencyRegistration(con);
                break;

            case 2:
                normalRegistration(con);
                break;

            case 3:
                return;

            default:
                cout << "Invalid choice!\n";
        }
    }
}