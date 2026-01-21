#include "DBmanager.h"
#include <algorithm> 

DatabaseManager::DatabaseManager(const std::string& host, const std::string& user, 
                                 const std::string& password, const std::string& database) {
    try {
        sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
        connection.reset(driver->connect(host, user, password));
        connection->setSchema(database);
        
        createTables(); // Создаем таблицы, если их нет
        std::cout << "Database connected successfully!" << std::endl;
    } catch (sql::SQLException& e) {
        std::cerr << "DB Connection Error: " << e.what() << std::endl;
        throw;
    }
}

DatabaseManager::~DatabaseManager() {}

void DatabaseManager::createTables() {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::unique_ptr<sql::Statement> stmt(connection->createStatement());
        
        // Таблица пользователей
        stmt->execute("CREATE TABLE IF NOT EXISTS users ("
                      "id INT AUTO_INCREMENT PRIMARY KEY, "
                      "name VARCHAR(255) NOT NULL, "
                      "login VARCHAR(255) NOT NULL UNIQUE, "
                      "password VARCHAR(255) NOT NULL, "
                      "is_banned TINYINT(1) DEFAULT 0)");

        // Публичные сообщения
        stmt->execute("CREATE TABLE IF NOT EXISTS public_messages ("
                      "id INT AUTO_INCREMENT PRIMARY KEY, "
                      "sender_id INT, "
                      "sender_name VARCHAR(255), "
                      "message_text TEXT, "
                      "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");

        // Приватные сообщения
        stmt->execute("CREATE TABLE IF NOT EXISTS private_messages ("
                      "id INT AUTO_INCREMENT PRIMARY KEY, "
                      "sender_id INT, "
                      "recipient_id INT, "
                      "message_text TEXT, "
                      "timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
                      "FOREIGN KEY (sender_id) REFERENCES users(id) ON DELETE CASCADE, "
                      "FOREIGN KEY (recipient_id) REFERENCES users(id) ON DELETE CASCADE)");
                      
    } catch (sql::SQLException& e) {
        std::cerr << "Error creating tables: " << e.what() << std::endl;
    }
}

// Пользователи
bool DatabaseManager::registerUser(const std::string& name, const std::string& login, const std::string& password) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        // Хешируем пароль 
        std::string hashedPassword = MySha::sha1(password); 

        std::unique_ptr<sql::PreparedStatement> stmt(
            connection->prepareStatement("INSERT INTO users(name, login, password) VALUES (?, ?, ?)")
        );
        stmt->setString(1, name);
        stmt->setString(2, login);
        stmt->setString(3, hashedPassword);
        stmt->execute();
        return true;
    } catch (sql::SQLException& e) {
        std::cerr << "Register Error: " << e.what() << std::endl;
        return false;
    }
}

std::shared_ptr<User> DatabaseManager::authenticateUser(const std::string& login, const std::string& password) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::string hashedPassword = MySha::sha1(password);

        std::unique_ptr<sql::PreparedStatement> stmt(
            connection->prepareStatement("SELECT id, name, login FROM users WHERE login = ? AND password = ? AND is_banned = 0")
        );
        stmt->setString(1, login);
        stmt->setString(2, hashedPassword);
        
        std::unique_ptr<sql::ResultSet> res(stmt->executeQuery());
        if (res->next()) {
            auto user = std::make_shared<User>();
            user->id = res->getInt("id");
            user->name = res->getString("name");
            user->login = res->getString("login");
            user->clientSocket = -1;
            return user;
        }
    } catch (sql::SQLException& e) {
        std::cerr << "Auth Error: " << e.what() << std::endl;
    }
    return nullptr;
}

std::vector<User> DatabaseManager::getAllUsers() {
    std::lock_guard<std::mutex> lock(dbMutex);
    std::vector<User> users;
    try {
        std::unique_ptr<sql::Statement> stmt(connection->createStatement());
        std::unique_ptr<sql::ResultSet> res(stmt->executeQuery("SELECT id, name, login FROM users WHERE is_banned = 0 ORDER BY name"));
        while (res->next()) {
            User u;
            u.id = res->getInt("id");
            u.name = res->getString("name");
            u.login = res->getString("login");
            u.clientSocket = -1;
            users.push_back(u);
        }
    } catch (sql::SQLException& e) {
        std::cerr << "getAllUsers Error: " << e.what() << std::endl;
    }
    return users;
}

std::vector<User> DatabaseManager::getBannedUsers() {
    std::lock_guard<std::mutex> lock(dbMutex);
    std::vector<User> users;
    try {
        std::unique_ptr<sql::Statement> stmt(connection->createStatement());
        std::unique_ptr<sql::ResultSet> res(stmt->executeQuery("SELECT id, name, login FROM users WHERE is_banned = 1"));
        while (res->next()) {
            User u;
            u.id = res->getInt("id");
            u.name = res->getString("name");
            u.login = res->getString("login");
            users.push_back(u);
        }
    } catch (sql::SQLException& e) {}
    return users;
}

bool DatabaseManager::banUser(const std::string& login) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(connection->prepareStatement("UPDATE users SET is_banned = 1 WHERE login = ?"));
        stmt->setString(1, login);
        int rows = stmt->executeUpdate();
        return rows > 0;
    } catch (sql::SQLException&) { return false; }
}

bool DatabaseManager::unbanUser(const std::string& login) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(connection->prepareStatement("UPDATE users SET is_banned = 0 WHERE login = ?"));
        stmt->setString(1, login);
        int rows = stmt->executeUpdate();
        return rows > 0;
    } catch (sql::SQLException&) { return false; }
}

bool DatabaseManager::deleteUser(const std::string& login) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(connection->prepareStatement("DELETE FROM users WHERE login = ?"));
        stmt->setString(1, login);
        int rows = stmt->executeUpdate();
        return rows > 0;
    } catch (sql::SQLException&) { return false; }
}

bool DatabaseManager::isUserBanned(const std::string& login) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(connection->prepareStatement("SELECT id FROM users WHERE login = ? AND is_banned = 1"));
        stmt->setString(1, login);
        std::unique_ptr<sql::ResultSet> res(stmt->executeQuery());
        return res->next();
    } catch (sql::SQLException&) { return false; }
}

int DatabaseManager::findUserIdByName(const std::string& name) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(connection->prepareStatement("SELECT id FROM users WHERE name = ?"));
        stmt->setString(1, name);
        std::unique_ptr<sql::ResultSet> res(stmt->executeQuery());
        if(res->next()) return res->getInt("id");
    } catch (sql::SQLException&) {}
    return -1;
}

// Сообщения
bool DatabaseManager::savePrivateMessage(int senderId, int recipientId, const std::string& messageText) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(
            connection->prepareStatement("INSERT INTO private_messages(sender_id, recipient_id, message_text) VALUES (?, ?, ?)")
        );
        stmt->setInt(1, senderId);
        stmt->setInt(2, recipientId);
        stmt->setString(3, messageText);
        stmt->execute();
        return true;
    } catch (sql::SQLException& e) {
        std::cerr << "SavePrivateMsg Error: " << e.what() << std::endl;
        return false;
    }
}

std::vector<Message> DatabaseManager::getPrivateMessages(const std::string& userLogin) {
    std::lock_guard<std::mutex> lock(dbMutex);
    std::vector<Message> messages;
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(connection->prepareStatement(
            "SELECT u1.name AS sender, u2.name AS recipient, pm.message_text "
            "FROM private_messages pm "
            "JOIN users u1 ON pm.sender_id = u1.id "
            "JOIN users u2 ON pm.recipient_id = u2.id "
            "WHERE u1.login = ? OR u2.login = ? "
            "ORDER BY pm.timestamp DESC LIMIT 50"
        ));
        stmt->setString(1, userLogin);
        stmt->setString(2, userLogin);
        std::unique_ptr<sql::ResultSet> res(stmt->executeQuery());
        
        while(res->next()) {
            Message msg;
            msg.from = res->getString("sender");
            msg.to = res->getString("recipient");
            msg.text = res->getString("message_text");
            messages.push_back(msg);
        }
        std::reverse(messages.begin(), messages.end()); // Разворачиваем для хронологии
    } catch (sql::SQLException& e) {
        std::cerr << "GetPrivateMsg Error: " << e.what() << std::endl;
    }
    return messages;
}

bool DatabaseManager::savePublicMessage(int senderId, const std::string& senderName, const std::string& messageText) {
    std::lock_guard<std::mutex> lock(dbMutex);
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(
            connection->prepareStatement("INSERT INTO public_messages(sender_id, sender_name, message_text) VALUES (?, ?, ?)")
        );
        stmt->setInt(1, senderId);
        stmt->setString(2, senderName);
        stmt->setString(3, messageText);
        stmt->execute();
        return true;
    } catch (sql::SQLException&) { return false; }
}

std::vector<Message> DatabaseManager::getPublicMessages(int limit) {
    std::lock_guard<std::mutex> lock(dbMutex);
    std::vector<Message> messages;
    try {
        std::unique_ptr<sql::PreparedStatement> stmt(
            connection->prepareStatement("SELECT sender_name, message_text FROM public_messages ORDER BY timestamp DESC LIMIT ?")
        );
        stmt->setInt(1, limit);
        std::unique_ptr<sql::ResultSet> res(stmt->executeQuery());
        
        while(res->next()) {
            Message msg;
            msg.from = res->getString("sender_name");
            msg.text = res->getString("message_text");
            messages.push_back(msg);
        }
        std::reverse(messages.begin(), messages.end());
    } catch (sql::SQLException&) {}
    return messages;
}
