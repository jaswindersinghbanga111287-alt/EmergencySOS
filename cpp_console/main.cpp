#include <iostream>
#include <mysql/jdbc.h>

using namespace std;


// ============================================================
// FUNCTION DECLARATIONS
// ============================================================

sql::Connection* connectDatabase();

void normalRegistration(sql::Connection* con);
void emergencyRegistration(sql::Connection* con);
void registrationMenu(sql::Connection* con);
void searchPatient(sql::Connection* con);
void updatePatientContact(sql::Connection* con);
void dischargePatient(sql::Connection* con);
void viewResources(sql::Connection* con);
void allocateResources(sql::Connection* con);
void releaseAllocation(sql::Connection* con);
void viewEmergencyQueue(sql::Connection* con);
void addPatientRecord(sql::Connection* con);
void viewPatientRecords(sql::Connection* con);

void addToEmergencyQueue(sql::Connection* con);
void completePatientTreatment(sql::Connection* con);


// ============================================================
// MAIN
// ============================================================

int main()
{
    sql::Connection* con = nullptr;

    try
    {
        con = connectDatabase();

        cout << "\n========================================\n";
        cout << "        EMERGENCY SOS SYSTEM\n";
        cout << "========================================\n";


        int choice;

        while (true)
        {
            cout << "\n===== MAIN MENU =====\n";
cout << "1. Patient Registration\n";
cout << "2. Add Patient Record\n";
cout << "3. View Patient Records\n";
cout << "4. Add Patient to Emergency Queue\n";
cout << "5. Complete Patient Treatment\n";
cout << "6. Search Patient\n";
cout << "7. Update Patient Contact\n";
cout << "8. View Emergency Queue\n";
cout << "9. View Hospital Resources\n";
cout << "10. Allocate Resources\n";
cout << "11. Release Resource Allocation\n";
cout << "12. Discharge Patient\n";
cout << "13. Exit\n";

            cout << "\nEnter choice: ";
            cin >> choice;


            switch (choice)
            {
                case 1: registrationMenu(con); break;
case 2: addPatientRecord(con); break;
case 3: viewPatientRecords(con); break;
case 4: addToEmergencyQueue(con); break;
case 5: completePatientTreatment(con); break;
case 6: searchPatient(con); break;
case 7: updatePatientContact(con); break;
case 8: viewEmergencyQueue(con); break;
case 9: viewResources(con); break;
case 10: allocateResources(con); break;
case 11: releaseAllocation(con); break;
case 12: dischargePatient(con); break;
case 13:
    cout << "Exiting EmergencySOS...\n";
    delete con;
    
                    return 0;

                default:
                    cout << "\nInvalid choice!\n";
            }
        }
    }
    catch (sql::SQLException& e)
    {
        cout << "\nDatabase connection failed!\n";
        cout << e.what() << "\n";

        if (con != nullptr)
        {
            delete con;
        }

        return 1;
    }

    return 0;
}