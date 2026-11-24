#include <iostream>
#include <string>
#include <mysql/jdbc.h>

using namespace std;


// ============================================================
// ADD PATIENT RECORD
// ============================================================

void addPatientRecord(sql::Connection* con)
{
    string aadhaar;
    string symptoms;
    string diagnosis;
    string treatment;
    string admissionStatus;

    cout << "\n===== ADD PATIENT RECORD =====\n";

    cout << "Enter Aadhaar: ";
    cin >> aadhaar;

    cin.ignore();

    try
    {
        sql::PreparedStatement* findStmt =
            con->prepareStatement(
                "SELECT patient_id "
                "FROM patients "
                "WHERE aadhaar = ?"
            );

        findStmt->setString(1, aadhaar);

        sql::ResultSet* res =
            findStmt->executeQuery();

        if (!res->next())
        {
            cout << "\nPatient not found!\n";

            delete res;
            delete findStmt;
            return;
        }

        int patientId =
            res->getInt("patient_id");

        delete res;
        delete findStmt;


        cout << "Enter Symptoms: ";
        getline(cin, symptoms);

        cout << "Enter Diagnosis: ";
        getline(cin, diagnosis);

        cout << "Enter Treatment: ";
        getline(cin, treatment);

        cout << "Enter Admission Status: ";
        getline(cin, admissionStatus);


        sql::PreparedStatement* pstmt =
            con->prepareStatement(
                "INSERT INTO patient_records "
                "(patient_id, symptoms, diagnosis, treatment, admission_status) "
                "VALUES (?, ?, ?, ?, ?)"
            );

        pstmt->setInt(1, patientId);
        pstmt->setString(2, symptoms);
        pstmt->setString(3, diagnosis);
        pstmt->setString(4, treatment);
        pstmt->setString(5, admissionStatus);

        pstmt->executeUpdate();

        delete pstmt;

        cout << "\nPatient record added successfully!\n";
    }
    catch (sql::SQLException& e)
    {
        cout << "\nFailed to add patient record!\n";
        cout << e.what() << "\n";
    }
}


// ============================================================
// VIEW PATIENT RECORDS
// ============================================================

void viewPatientRecords(sql::Connection* con)
{
    int patientId;

    cout << "\n===== VIEW PATIENT RECORDS =====\n";

    try
    {
        sql::Statement* stmt =
            con->createStatement();

        sql::ResultSet* patients =
            stmt->executeQuery(
                "SELECT patient_id, aadhaar, name "
                "FROM patients "
                "ORDER BY patient_id"
            );

        cout << "\nAvailable Patients:\n";

        while (patients->next())
        {
            cout << "Patient ID: "
                 << patients->getInt("patient_id")
                 << " | Aadhaar: "
                 << patients->getString("aadhaar")
                 << " | Name: "
                 << patients->getString("name")
                 << "\n";
        }

        delete patients;
        delete stmt;


        cout << "\nEnter Patient ID: ";
        cin >> patientId;


        sql::PreparedStatement* patientStmt =
            con->prepareStatement(
                "SELECT patient_id, aadhaar, name, age, "
                "gender, contact, address, registration_time "
                "FROM patients "
                "WHERE patient_id = ?"
            );

        patientStmt->setInt(1, patientId);

        sql::ResultSet* patient =
            patientStmt->executeQuery();

        if (!patient->next())
        {
            cout << "\nPatient not found!\n";

            delete patient;
            delete patientStmt;
            return;
        }


        cout << "\n===== PATIENT INFORMATION =====\n";

        cout << "Patient ID: "
             << patient->getInt("patient_id") << "\n";

        cout << "Aadhaar: "
             << patient->getString("aadhaar") << "\n";

        cout << "Name: "
             << patient->getString("name") << "\n";

        cout << "Age: "
             << patient->getInt("age") << "\n";

        cout << "Gender: "
             << patient->getString("gender") << "\n";

        cout << "Contact: "
             << patient->getString("contact") << "\n";

        cout << "Address: "
             << patient->getString("address") << "\n";

        cout << "Registration Time: "
             << patient->getString("registration_time") << "\n";


        delete patient;
        delete patientStmt;


        sql::PreparedStatement* recordStmt =
            con->prepareStatement(
                "SELECT record_id, symptoms, diagnosis, "
                "treatment, admission_status, record_date "
                "FROM patient_records "
                "WHERE patient_id = ? "
                "ORDER BY record_date DESC"
            );

        recordStmt->setInt(1, patientId);

        sql::ResultSet* records =
            recordStmt->executeQuery();


        cout << "\n===== MEDICAL RECORDS =====\n";

        bool found = false;

        while (records->next())
        {
            found = true;

            cout << "\nRecord ID: "
                 << records->getInt("record_id") << "\n";

            cout << "Symptoms: "
                 << records->getString("symptoms") << "\n";

            cout << "Diagnosis: "
                 << records->getString("diagnosis") << "\n";

            cout << "Treatment: "
                 << records->getString("treatment") << "\n";

            cout << "Admission Status: "
                 << records->getString("admission_status") << "\n";

            cout << "Record Date: "
                 << records->getString("record_date") << "\n";
        }

        if (!found)
        {
            cout << "No medical records found.\n";
        }

        delete records;
        delete recordStmt;
    }
    catch (sql::SQLException& e)
    {
        cout << "\nFailed to view patient records!\n";
        cout << e.what() << "\n";
    }
}