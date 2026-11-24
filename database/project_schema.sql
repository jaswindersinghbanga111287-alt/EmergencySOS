CREATE DATABASE IF NOT EXISTS emergency_sos;
USE emergency_sos;

-- Keep patient_id as the primary key because all C++ modules and foreign keys use it.
CREATE TABLE IF NOT EXISTS patients (
    patient_id INT AUTO_INCREMENT PRIMARY KEY,
    aadhaar VARCHAR(12) NULL UNIQUE,
    name VARCHAR(100) NULL,
    age INT NULL,
    gender VARCHAR(20) NULL,
    contact VARCHAR(15) NULL,
    address VARCHAR(255) NULL,
    registration_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS patient_records (
    record_id INT AUTO_INCREMENT PRIMARY KEY,
    patient_id INT NOT NULL,
    symptoms TEXT,
    diagnosis TEXT,
    treatment TEXT,
    admission_status VARCHAR(30) DEFAULT 'Waiting',
    record_date TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT fk_patient_records_patient
      FOREIGN KEY (patient_id) REFERENCES patients(patient_id)
      ON DELETE CASCADE ON UPDATE CASCADE
);

CREATE TABLE IF NOT EXISTS emergency_queue (
    queue_id INT AUTO_INCREMENT PRIMARY KEY,
    patient_id INT NOT NULL,
    priority INT NOT NULL,
    arrival_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    queue_status ENUM('Waiting','In Treatment','Completed','Cancelled') DEFAULT 'Waiting',
    CONSTRAINT fk_emergency_queue_patient
      FOREIGN KEY (patient_id) REFERENCES patients(patient_id)
      ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT chk_priority CHECK (priority BETWEEN 1 AND 4)
);

CREATE TABLE IF NOT EXISTS beds (
    bed_id INT AUTO_INCREMENT PRIMARY KEY,
    bed_number VARCHAR(20) NOT NULL UNIQUE,
    ward VARCHAR(50) NOT NULL,
    bed_type VARCHAR(50) NOT NULL,
    bed_status ENUM('Available','Occupied','Maintenance') DEFAULT 'Available'
);

CREATE TABLE IF NOT EXISTS staff (
    staff_id INT AUTO_INCREMENT PRIMARY KEY,
    staff_name VARCHAR(100) NOT NULL,
    role ENUM('Doctor','Nurse','Technician') NOT NULL,
    specialization VARCHAR(100),
    staff_status ENUM('Available','Busy','Off Duty') DEFAULT 'Available'
);

CREATE TABLE IF NOT EXISTS equipment (
    equipment_id INT AUTO_INCREMENT PRIMARY KEY,
    equipment_name VARCHAR(100) NOT NULL,
    total_quantity INT NOT NULL DEFAULT 0,
    available_quantity INT NOT NULL DEFAULT 0,
    equipment_status ENUM('Available','In Use','Maintenance') DEFAULT 'Available',
    CONSTRAINT chk_equipment_quantity CHECK (
      total_quantity >= 0 AND available_quantity >= 0 AND available_quantity <= total_quantity
    )
);

CREATE TABLE IF NOT EXISTS resource_allocations (
    allocation_id INT AUTO_INCREMENT PRIMARY KEY,
    patient_id INT NOT NULL,
    bed_id INT NULL,
    staff_id INT NULL,
    equipment_id INT NULL,
    priority INT NULL,                       -- severity used for the release timer (1 Critical .. 4 Low)
    allocation_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    release_due_at TIMESTAMP NULL,           -- when the auto-release timer runs out
    release_time TIMESTAMP NULL,
    release_type VARCHAR(20) NULL,           -- 'Manual' or 'Automatic'
    allocation_status ENUM('Active','Released') DEFAULT 'Active',
    FOREIGN KEY (patient_id) REFERENCES patients(patient_id),
    FOREIGN KEY (bed_id) REFERENCES beds(bed_id),
    FOREIGN KEY (staff_id) REFERENCES staff(staff_id),
    FOREIGN KEY (equipment_id) REFERENCES equipment(equipment_id)
);

INSERT IGNORE INTO beds (bed_number, ward, bed_type) VALUES
('ER-01','Emergency','Standard'),
('ER-02','Emergency','ICU'),
('ER-03','Emergency','Standard'),
('GW-01','General Ward','Standard'),
('GW-02','General Ward','Standard');

INSERT IGNORE INTO staff (staff_name, role, specialization, staff_status) VALUES
('Dr. Sharma','Doctor','Emergency Medicine','Available'),
('Dr. Verma','Doctor','Cardiology','Available'),
('Nurse A','Nurse','Emergency Care','Available'),
('Nurse B','Nurse','ICU','Available'),
('Technician A','Technician','Equipment Support','Available');

INSERT IGNORE INTO equipment (equipment_name,total_quantity,available_quantity,equipment_status) VALUES
('Oxygen Cylinder',10,10,'Available'),
('Wheelchair',5,5,'Available'),
('Cardiac Monitor',4,4,'Available'),
('Ventilator',3,3,'Available'),
('Defibrillator',2,2,'Available');


-- DBMS concept objects (audit_log table, views, stored function/procedures, triggers and
-- indexes) are created automatically by the C++ server at start-up.
-- See backend/src/dbms_concepts.hpp -> installDbmsObjects().
