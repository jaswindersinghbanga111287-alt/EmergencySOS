#include <iostream>
#include <string>
#include <limits>
#include <mysql/jdbc.h>
using namespace std;

// Add these declarations to main.cpp:
// void viewResources(sql::Connection*);
// void allocateResources(sql::Connection*);
// void releaseAllocation(sql::Connection*);

void viewResources(sql::Connection* con)
{
    try {
        sql::Statement* s = con->createStatement();
        sql::ResultSet* r = s->executeQuery(
            "SELECT bed_id,bed_number,ward,bed_type,bed_status FROM beds ORDER BY bed_id");
        cout << "\n--- BEDS ---\n";
        while (r->next()) cout << r->getInt("bed_id") << " | "
            << r->getString("bed_number") << " | " << r->getString("ward")
            << " | " << r->getString("bed_type") << " | " << r->getString("bed_status") << "\n";
        delete r;

        r = s->executeQuery("SELECT staff_id,staff_name,role,specialization,staff_status FROM staff ORDER BY staff_id");
        cout << "\n--- STAFF ---\n";
        while (r->next()) cout << r->getInt("staff_id") << " | "
            << r->getString("staff_name") << " | " << r->getString("role")
            << " | " << r->getString("specialization") << " | " << r->getString("staff_status") << "\n";
        delete r;

        r = s->executeQuery("SELECT equipment_id,equipment_name,total_quantity,available_quantity,equipment_status FROM equipment ORDER BY equipment_id");
        cout << "\n--- EQUIPMENT ---\n";
        while (r->next()) cout << r->getInt("equipment_id") << " | "
            << r->getString("equipment_name") << " | total="
            << r->getInt("total_quantity") << " | available="
            << r->getInt("available_quantity") << " | " << r->getString("equipment_status") << "\n";
        delete r; delete s;
    } catch (sql::SQLException& e) { cout << "Could not load resources: " << e.what() << "\n"; }
}

void allocateResources(sql::Connection* con)
{
    int patientId, bedId, staffId, equipmentId;
    cout << "\nPatient ID: ";
    if (!(cin >> patientId)) { cin.clear(); cin.ignore(numeric_limits<streamsize>::max(), '\n'); return; }
    cout << "Bed ID (0 for none): "; cin >> bedId;
    cout << "Staff ID (0 for none): "; cin >> staffId;
    cout << "Equipment ID (0 for none): "; cin >> equipmentId;
    if (bedId < 0 || staffId < 0 || equipmentId < 0 || (bedId==0 && staffId==0 && equipmentId==0)) {
        cout << "Choose at least one resource and use IDs 0 or greater.\n"; return;
    }

    try {
        con->setAutoCommit(false);
        sql::PreparedStatement* p = con->prepareStatement("SELECT patient_id FROM patients WHERE patient_id=? FOR UPDATE");
        p->setInt(1, patientId); sql::ResultSet* pr = p->executeQuery();
        if (!pr->next()) {
            delete pr; delete p; con->rollback(); con->setAutoCommit(true);
            cout << "Patient does not exist.\n"; return;
        }
        delete pr; delete p;

        if (bedId > 0) {
            p = con->prepareStatement("SELECT bed_status FROM beds WHERE bed_id=? FOR UPDATE");
            p->setInt(1, bedId); pr = p->executeQuery();
            if (!pr->next() || pr->getString("bed_status") != "Available") {
                delete pr; delete p; con->rollback(); con->setAutoCommit(true);
                cout << "Bed not found or not available.\n"; return;
            }
            delete pr; delete p;
        }
        if (staffId > 0) {
            p = con->prepareStatement("SELECT staff_status FROM staff WHERE staff_id=? FOR UPDATE");
            p->setInt(1, staffId); pr = p->executeQuery();
            if (!pr->next() || pr->getString("staff_status") != "Available") {
                delete pr; delete p; con->rollback(); con->setAutoCommit(true);
                cout << "Staff member not found or not available.\n"; return;
            }
            delete pr; delete p;
        }
        if (equipmentId > 0) {
            p = con->prepareStatement("SELECT available_quantity FROM equipment WHERE equipment_id=? FOR UPDATE");
            p->setInt(1, equipmentId); pr = p->executeQuery();
            if (!pr->next() || pr->getInt("available_quantity") < 1) {
                delete pr; delete p; con->rollback(); con->setAutoCommit(true);
                cout << "Equipment not found or none available.\n"; return;
            }
            delete pr; delete p;
        }

        p = con->prepareStatement(
            "INSERT INTO resource_allocations(patient_id,bed_id,staff_id,equipment_id) VALUES(?,?,?,?)");
        p->setInt(1, patientId);
        if (bedId == 0) p->setNull(2, sql::DataType::INTEGER); else p->setInt(2, bedId);
        if (staffId == 0) p->setNull(3, sql::DataType::INTEGER); else p->setInt(3, staffId);
        if (equipmentId == 0) p->setNull(4, sql::DataType::INTEGER); else p->setInt(4, equipmentId);
        p->executeUpdate(); delete p;

        if (bedId > 0) {
            p = con->prepareStatement("UPDATE beds SET bed_status='Occupied' WHERE bed_id=?");
            p->setInt(1, bedId); p->executeUpdate(); delete p;
        }
        if (staffId > 0) {
            p = con->prepareStatement("UPDATE staff SET staff_status='Busy' WHERE staff_id=?");
            p->setInt(1, staffId); p->executeUpdate(); delete p;
        }
        if (equipmentId > 0) {
            p = con->prepareStatement(
                "UPDATE equipment SET equipment_status=IF(available_quantity=1,'In Use','Available'), "
                "available_quantity=available_quantity-1 WHERE equipment_id=?");
            p->setInt(1, equipmentId); p->executeUpdate(); delete p;
        }
        con->commit(); con->setAutoCommit(true);
        cout << "Resources allocated successfully.\n";
    } catch (sql::SQLException& e) {
        try { con->rollback(); con->setAutoCommit(true); } catch (...) {}
        cout << "Allocation failed; transaction rolled back: " << e.what() << "\n";
    }
}

void releaseAllocation(sql::Connection* con)
{
    int id;
    cout << "\nAllocation ID to release: ";
    if (!(cin >> id)) { cin.clear(); cin.ignore(numeric_limits<streamsize>::max(), '\n'); return; }

    try {
        con->setAutoCommit(false);
        sql::PreparedStatement* p = con->prepareStatement(
            "SELECT bed_id,staff_id,equipment_id FROM resource_allocations "
            "WHERE allocation_id=? AND allocation_status='Active' FOR UPDATE");
        p->setInt(1, id); sql::ResultSet* r = p->executeQuery();
        if (!r->next()) {
            delete r; delete p; con->rollback(); con->setAutoCommit(true);
            cout << "Active allocation not found.\n"; return;
        }
        int bed = r->getInt("bed_id"); bool noBed = r->isNull("bed_id");
        int staff = r->getInt("staff_id"); bool noStaff = r->isNull("staff_id");
        int equipment = r->getInt("equipment_id"); bool noEquipment = r->isNull("equipment_id");
        delete r; delete p;

        if (!noBed) { p=con->prepareStatement("UPDATE beds SET bed_status='Available' WHERE bed_id=?"); p->setInt(1,bed); p->executeUpdate(); delete p; }
        if (!noStaff) { p=con->prepareStatement("UPDATE staff SET staff_status='Available' WHERE staff_id=? AND staff_status='Busy'"); p->setInt(1,staff); p->executeUpdate(); delete p; }
        if (!noEquipment) {
            p=con->prepareStatement("UPDATE equipment SET available_quantity=LEAST(total_quantity,available_quantity+1), equipment_status='Available' WHERE equipment_id=?");
            p->setInt(1,equipment); p->executeUpdate(); delete p;
        }
        p=con->prepareStatement("UPDATE resource_allocations SET allocation_status='Released',release_time=NOW() WHERE allocation_id=?");
        p->setInt(1,id); p->executeUpdate(); delete p;
        con->commit(); con->setAutoCommit(true);
        cout << "Allocation released and resources returned.\n";
    } catch (sql::SQLException& e) {
        try { con->rollback(); con->setAutoCommit(true); } catch (...) {}
        cout << "Release failed; transaction rolled back: " << e.what() << "\n";
    }
}
