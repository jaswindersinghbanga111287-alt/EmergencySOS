#include <iostream>
#include <string>
#include <limits>
#include <mysql/jdbc.h>
using namespace std;

// Add these declarations to main.cpp:
// void searchPatient(sql::Connection*);
// void updatePatientContact(sql::Connection*);
// void dischargePatient(sql::Connection*);

void searchPatient(sql::Connection* con)
{
    int id;
    cout << "\nEnter Patient ID: ";
    if (!(cin >> id)) { cin.clear(); cin.ignore(numeric_limits<streamsize>::max(), '\n'); return; }

    try {
        sql::PreparedStatement* p = con->prepareStatement(
            "SELECT patient_id, aadhaar, name, age, gender, contact, address, registration_time "
            "FROM patients WHERE patient_id=?");
        p->setInt(1, id);
        sql::ResultSet* r = p->executeQuery();

        if (!r->next()) cout << "Patient not found.\n";
        else {
            cout << "\nPatient ID: " << r->getInt("patient_id")
                 << "\nName: " << r->getString("name")
                 << "\nAge: " << r->getInt("age")
                 << "\nGender: " << r->getString("gender")
                 << "\nContact: " << r->getString("contact")
                 << "\nAddress: " << r->getString("address")
                 << "\nRegistration: " << r->getString("registration_time") << "\n";
        }
        delete r; delete p;
    } catch (sql::SQLException& e) { cout << "Search failed: " << e.what() << "\n"; }
}

void updatePatientContact(sql::Connection* con)
{
    int id; string contact;
    cout << "\nPatient ID: ";
    if (!(cin >> id)) { cin.clear(); cin.ignore(numeric_limits<streamsize>::max(), '\n'); return; }
    cout << "New contact number: "; cin >> contact;

    try {
        sql::PreparedStatement* p = con->prepareStatement(
            "UPDATE patients SET contact=? WHERE patient_id=?");
        p->setString(1, contact); p->setInt(2, id);
        int changed = p->executeUpdate();
        cout << (changed ? "Contact updated.\n" : "Patient not found or contact unchanged.\n");
        delete p;
    } catch (sql::SQLException& e) { cout << "Update failed: " << e.what() << "\n"; }
}

void dischargePatient(sql::Connection* con)
{
    int id;
    cout << "\nPatient ID to discharge: ";
    if (!(cin >> id)) { cin.clear(); cin.ignore(numeric_limits<streamsize>::max(), '\n'); return; }

    try {
        con->setAutoCommit(false);
        sql::PreparedStatement* check = con->prepareStatement(
            "SELECT allocation_id, bed_id, staff_id, equipment_id "
            "FROM resource_allocations WHERE patient_id=? AND allocation_status='Active' FOR UPDATE");
        check->setInt(1, id);
        sql::ResultSet* r = check->executeQuery();

        while (r->next()) {
            int allocationId = r->getInt("allocation_id");
            int bedId = r->getInt("bed_id");
            bool bedNull = r->isNull("bed_id");
            int staffId = r->getInt("staff_id");
            bool staffNull = r->isNull("staff_id");
            int equipmentId = r->getInt("equipment_id");
            bool equipmentNull = r->isNull("equipment_id");

            if (!bedNull) {
                sql::PreparedStatement* b = con->prepareStatement("UPDATE beds SET bed_status='Available' WHERE bed_id=?");
                b->setInt(1, bedId); b->executeUpdate(); delete b;
            }
            if (!staffNull) {
                sql::PreparedStatement* s = con->prepareStatement("UPDATE staff SET staff_status='Available' WHERE staff_id=? AND staff_status='Busy'");
                s->setInt(1, staffId); s->executeUpdate(); delete s;
            }
            if (!equipmentNull) {
                sql::PreparedStatement* e = con->prepareStatement(
                    "UPDATE equipment SET available_quantity=LEAST(total_quantity, available_quantity+1) WHERE equipment_id=?");
                e->setInt(1, equipmentId); e->executeUpdate(); delete e;
            }
            sql::PreparedStatement* a = con->prepareStatement(
                "UPDATE resource_allocations SET allocation_status='Released', release_time=NOW() WHERE allocation_id=?");
            a->setInt(1, allocationId); a->executeUpdate(); delete a;
        }
        delete r; delete check;

        sql::PreparedStatement* q = con->prepareStatement(
            "UPDATE emergency_queue SET queue_status='Completed' WHERE patient_id=? AND queue_status IN ('Waiting','In Treatment')");
        q->setInt(1, id); q->executeUpdate(); delete q;

        sql::PreparedStatement* records = con->prepareStatement(
            "UPDATE patient_records SET admission_status='Discharged' WHERE patient_id=? AND admission_status IN ('Admitted','Waiting')");
        records->setInt(1, id); records->executeUpdate(); delete records;

        con->commit();
        con->setAutoCommit(true);
        cout << "Discharge completed. Active allocations released.\n";
    } catch (sql::SQLException& e) {
        try { con->rollback(); con->setAutoCommit(true); } catch (...) {}
        cout << "Discharge failed; changes rolled back: " << e.what() << "\n";
    }
}
