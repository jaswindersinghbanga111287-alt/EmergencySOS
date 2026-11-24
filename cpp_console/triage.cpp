#include <iostream>
#include <string>
#include <mysql/jdbc.h>

using namespace std;


// ============================================================
// ADD PATIENT TO EMERGENCY QUEUE
// ============================================================

void addToEmergencyQueue(sql::Connection* con)
{
    int patientId;
    int priority;

    cout << "\n===== ADD TO EMERGENCY QUEUE =====\n";

    cout << "Enter Patient ID: ";
    cin >> patientId;

    try
    {
        // Check patient
        sql::PreparedStatement* patientStmt =
            con->prepareStatement(
                "SELECT patient_id "
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

        delete patient;
        delete patientStmt;


        // Check existing waiting entry
        sql::PreparedStatement* queueCheck =
            con->prepareStatement(
                "SELECT queue_id "
                "FROM emergency_queue "
                "WHERE patient_id = ? "
                "AND queue_status = 'Waiting'"
            );

        queueCheck->setInt(1, patientId);

        sql::ResultSet* existing =
            queueCheck->executeQuery();

        if (existing->next())
        {
            cout << "\nPatient is already in the emergency queue!\n";

            delete existing;
            delete queueCheck;
            return;
        }

        delete existing;
        delete queueCheck;


        cout << "\nPriority Levels:\n";
        cout << "1. Critical\n";
        cout << "2. High\n";
        cout << "3. Medium\n";
        cout << "4. Low\n";

        cout << "Enter Priority: ";
        cin >> priority;


        if (priority < 1 || priority > 4)
        {
            cout << "\nInvalid priority!\n";
            return;
        }


        sql::PreparedStatement* insertStmt =
            con->prepareStatement(
                "INSERT INTO emergency_queue "
                "(patient_id, priority) "
                "VALUES (?, ?)"
            );

        insertStmt->setInt(1, patientId);
        insertStmt->setInt(2, priority);

        insertStmt->executeUpdate();

        delete insertStmt;


        cout << "\nPatient added to emergency queue!\n";

        cout << "Priority: ";

        switch (priority)
        {
            case 1:
                cout << "Critical";
                break;

            case 2:
                cout << "High";
                break;

            case 3:
                cout << "Medium";
                break;

            case 4:
                cout << "Low";
                break;
        }

        cout << "\nStatus: Waiting\n";
    }
    catch (sql::SQLException& e)
    {
        cout << "\nFailed to add patient to queue!\n";
        cout << e.what() << "\n";
    }
}


// ============================================================
// COMPLETE PATIENT TREATMENT
// ============================================================

void completePatientTreatment(sql::Connection* con)
{
    int patientId;

    cout << "\n===== COMPLETE PATIENT TREATMENT =====\n";

    cout << "Enter Patient ID: ";
    cin >> patientId;

    try
    {
        sql::PreparedStatement* pstmt =
            con->prepareStatement(
                "SELECT queue_id, priority, queue_status "
                "FROM emergency_queue "
                "WHERE patient_id = ? "
                "ORDER BY arrival_time DESC "
                "LIMIT 1"
            );

        pstmt->setInt(1, patientId);

        sql::ResultSet* res =
            pstmt->executeQuery();


        if (!res->next())
        {
            cout << "\nPatient is not in the emergency queue!\n";

            delete res;
            delete pstmt;
            return;
        }


        int queueId =
            res->getInt("queue_id");

        string status =
            res->getString("queue_status");


        if (status == "Completed")
        {
            cout << "\nPatient treatment is already completed!\n";

            delete res;
            delete pstmt;
            return;
        }


        if (status != "Waiting")
        {
            cout << "\nPatient cannot be marked as completed.\n";
            cout << "Current status: "
                 << status << "\n";

            delete res;
            delete pstmt;
            return;
        }


        delete res;
        delete pstmt;


        sql::PreparedStatement* updateStmt =
            con->prepareStatement(
                "UPDATE emergency_queue "
                "SET queue_status = 'Completed' "
                "WHERE queue_id = ?"
            );

        updateStmt->setInt(1, queueId);

        updateStmt->executeUpdate();

        delete updateStmt;


        cout << "\nPatient treatment completed successfully!\n";
        cout << "Queue status: Completed\n";
    }
    catch (sql::SQLException& e)
    {
        cout << "\nFailed to complete treatment!\n";
        cout << e.what() << "\n";
    }
}