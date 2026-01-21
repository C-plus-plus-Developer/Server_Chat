#pragma once

#include <memory> ---
#include <vector>
#include <string>
#include <mutex>
#include <iostream>
#include <optional>

#include <mysql_driver.h>
#include <mysql_connection.h>
#include <cppconn/statement.h>
#include <cppconn/resultset.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>

#include "sha1.h" 
#include "User.h"
#include "Messages.h" 

class DatabaseManager {
private:
    std::mutex dbMutex; // Мьютекс для защиты соединения
    std::unique_ptr<sql::Connection> connection;

    // Внутренний метод для создания таблиц при старте
    void createTables();

public:
    DatabaseManager(const std::string& host, const std::string& user, 
                    const std::string& password, const std::string& database);
    ~DatabaseManager();

    // Пользователи 
    bool registerUser(const std::string& name, const std::string& login, const std::string& password);
    std::shared_ptr<User> authenticateUser(const std::string& login, const std::string& password);
    
    std::vector<User> getAllUsers();
    std::vector<User> getBannedUsers();
    
    bool banUser(const std::string& login);
    bool unbanUser(const std::string& login);
    bool deleteUser(const std::string& login);
    bool isUserBanned(const std::string& login);
    
    int findUserIdByName(const std::string& name);
    int findUserIdByLogin(const std::string& login);

    // Сообщения:
    // Приватные
    bool savePrivateMessage(int senderId, int recipientId, const std::string& messageText);
    std::vector<Message> getPrivateMessages(const std::string& userLogin);
    
    // Публичные
    bool savePublicMessage(int senderId, const std::string& senderName, const std::string& messageText);
    std::vector<Message> getPublicMessages(int limit = 50);
};
