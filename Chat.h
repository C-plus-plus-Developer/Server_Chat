#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <shared_mutex> 
#include <atomic>
#include <netinet/in.h>
#include <unordered_map>
#include <memory>
#include <sstream>
#include <algorithm>

#include "ThreadPool.h"
#include "DBmanager.h"
#include "User.h"
#include "Messages.h"

class Chat {
public:
    Chat();
    ~Chat();
    
    // Основной метод запуска сервера
    void setup();

private:
    ThreadPool pool;
    std::unique_ptr<DatabaseManager> dbManager;

    // Состояние сервера
    int serverSocket;
    std::atomic<int> activeConnections{0};
    std::atomic<int> totalConnections{0};

    // Храним shared_ptr, чтобы при реаллокации вектора указатели не ломались
    std::vector<std::shared_ptr<User>> users;
    mutable std::shared_mutex usersMutex; 
    
    // Хранит недочитанные команды для каждого сокета
    std::unordered_map<int, std::string> socketBuffers;
    std::mutex buffersMutex;

    const int PORT = 7777;
    const int MESSAGE_LENGTH = 4096;
    
    // Список администраторов 
    const std::unordered_map<std::string, std::string> adminAkk = {
        {"admin", "admin123"},
        {"moder", "moder123"}
    };

    enum class ReadResult { SUCCESS, DISCONNECTED, WOULD_BLOCK, ERROR };
    
    // Читает данные из сокета, склеивает пакеты и возвращает полную команду
    ReadResult readCommand(int socket, std::string& outCommand);
    
    //Потокобезопасные методы управления пользователями
    std::shared_ptr<User> getUserBySocket(int socket);
    std::shared_ptr<User> getUserByLogin(const std::string& login);
    std::shared_ptr<User> getUserByName(const std::string& name);
    
    void addOrUpdateUser(const User& user);
    
    // Атомарная операция проверки и входа (защита от двойного логина)
    bool tryLoginUser(const std::string& login, int clientSocket, User& loggedInUser);
    
    void removeUserBySocket(int socket);
    void removeUserByLogin(const std::string& login);
    void updateUserSocket(const std::string& login, int socket);
    
    // Возвращает копию списка пользователей 
    std::vector<User> getOnlineUsersCopy();

    // Логика обработки 
    void handleClient(int clientSocket);
    bool processCommand(int clientSocket, const std::string& command);
    
    // Функции чата 
    bool Login(const std::string& login, const std::string& password, int clientSocket, User& loggedInUser);
    void AddNewUser(const std::string& login, const std::string& password, const std::string& name, int clientSocket);
    
    //Принимает shared_ptr, чтобы объект не удалился, пока пользователь внутри панели
    void UserPanel(int clientSocket, std::shared_ptr<User> user);
    
    //Сообщения
    void SendMessage(const std::string& recipientName, const std::string& message, int senderSocket, const User& sender);
    void SendPublicMessage(const std::string& message, int senderSocket, const User& sender);
    
    void PrintPrivateMessage(int clientSocket, const User& user);
    void PrintPublicMessage(int clientSocket);
    void PrintAllUsers(int clientSocket);
    void PrintOnlineUsers(int clientSocket);

    // Админ панель 
    bool AdminLogin(const std::string& login, const std::string& password);
    void AdminPanel(int clientSocket);
    void banUser(const std::string& login, int adminSocket);
    void unbanUser(const std::string& login, int adminSocket);
    void deleteUser(const std::string& login, int adminSocket);
    void PrintBannedUsers(int clientSocket);

    // Сетевые методы
    bool sendToClient(int socket, const std::string& message);
    void sendResponse(int clientSocket, const std::string& response);
    void closeSocketSafe(int socket);
    void cleanupClient(int socket);

};
