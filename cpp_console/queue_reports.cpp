#include <iostream>
#include <mysql/jdbc.h>
using namespace std;

// Add this declaration to main.cpp: void viewEmergencyQueue(sql::Connection*);

void viewEmergencyQueue(sql::Connection* con)
{
    try {
        sql::Statement* s = con->createStatement();
        sql::ResultSet* r = s->executeQuery(
            "SELECT q.queue_id,q.patient_id,p.name,q.priority,q.arrival_time,q.queue_status "
            "FROM emergency_queue q JOIN patients p ON p.patient_id=q.patient_id "
            "WHERE q.queue_status IN ('Waiting','In Treatment') "
            "ORDER BY q.priority ASC,q.arrival_time ASC");

        cout << "\n===== ACTIVE EMERGENCY QUEUE =====\n";
        cout << "Priority: 1=Critical, 2=High, 3=Medium, 4=Low\n";
        bool found = false;
        while (r->next()) {
            found = true;
            cout << "Queue ID: " << r->getInt("queue_id")
                 << " | Patient ID: " << r->getInt("patient_id")
                 << " | Name: " << r->getString("name")
                 << " | Priority: " << r->getInt("priority")
                 << " | Arrival: " << r->getString("arrival_time")
                 << " | Status: " << r->getString("queue_status") << "\n";
        }
        if (!found) cout << "No patients are currently waiting or in treatment.\n";
        delete r; delete s;
    } catch (sql::SQLException& e) { cout << "Queue report failed: " << e.what() << "\n"; }
}
