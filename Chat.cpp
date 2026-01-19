#include "Chat.h"
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <sstream>

// ==========================================
// Конструктор и Деструктор
// ==========================================

Chat::Chat() : pool(50) {
    try {
        // Убедитесь, что параметры подключения верны
        dbManager = std::make_unique<DatabaseManager>("tcp://127.0.0.1:3306", "dbeaver", "dbeaver123", "bd");
    } catch (std::exception& e) {
        std::cerr << "[CRITICAL] Database connection failed: " << e.what() << std::endl;
        exit(1);
    }
}

Chat::~Chat() {
    closeSocketSafe(serverSocket);
}

// ==========================================
// Настройка и Запуск сервера
// ==========================================

void Chat::setup() {
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        std::cerr << "Socket creation failed!" << std::endl;
        exit(1);
    }

    // Неблокирующий режим для сервера
    int flags = fcntl(serverSocket, F_GETFL, 0);
    fcntl(serverSocket, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in serverAddress;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(PORT);
    serverAddress.sin_family = AF_INET;

    int opt = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "setsockopt SO_REUSEADDR failed!" << std::endl;
    }
    
    #ifdef SO_REUSEPORT
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        std::cerr << "setsockopt SO_REUSEPORT failed!" << std::endl;
    }
    #endif

    if (::bind(serverSocket, (sockaddr*)&serverAddress, sizeof(serverAddress)) == -1) {
        std::cerr << "Socket binding failed!" << std::endl;
        exit(1);
    }

    if (listen(serverSocket, SOMAXCONN) == -1) {
        std::cerr << "Listen failed!" << std::endl;
        exit(1);
    }
    
    std::cout << "Server listening on port " << PORT << "..." << std::endl;
    
    // Загружаем пользователей из БД в память (но они пока оффлайн)
    // Используем безопасный метод загрузки и конвертации
    auto dbUsers = dbManager->getAllUsers();
    {
        std::lock_guard<std::shared_mutex> lock(usersMutex);
        users.clear();
        for(const auto& u : dbUsers) {
            users.push_back(std::make_shared<User>(u));
        }
    }

    // Главный цикл приема соединений
    while (true) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        
        // Используем локальные переменные, чтобы избежать гонки данных!
        int newClientSocket = ::accept(serverSocket, (sockaddr*)&clientAddr, &addrLen);
        
        if (newClientSocket == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::cerr << "Accept error: " << strerror(errno) << std::endl;
            continue;
        }

        // Неблокирующий режим для клиента
        flags = fcntl(newClientSocket, F_GETFL, 0);
        fcntl(newClientSocket, F_SETFL, flags | O_NONBLOCK);

        activeConnections++;
        totalConnections++;
        
        std::cout << "New connection from " << inet_ntoa(clientAddr.sin_addr) 
                  << ":" << ntohs(clientAddr.sin_port) 
                  << " (socket: " << newClientSocket 
                  << ", active: " << activeConnections << ")" << std::endl;

        // Передаем обработку в пул потоков
        pool.enqueue([this, newClientSocket]() {
            this->handleClient(newClientSocket);
        });
    }
}

// ==========================================
// Сетевое взаимодействие (Чтение/Запись)
// ==========================================

void Chat::closeSocketSafe(int socket) {
    if (socket > 0) {
        shutdown(socket, SHUT_RDWR);
        close(socket);
    }
}

bool Chat::sendToClient(int socket, const std::string& message) {
    if(socket <= 0) return false;
    std::string messageWithNewline = message + "\n";
    size_t totalSent = 0;
    const char* data = messageWithNewline.c_str();
    size_t length = messageWithNewline.size();

    while(totalSent < length) {
        ssize_t sent = send(socket, data + totalSent, length - totalSent, MSG_NOSIGNAL);
        if(sent <= 0) {
            if(errno == EINTR) continue;
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            return false;
        }
        totalSent += sent;
    }
    return true;
}

void Chat::sendResponse(int clientSocket, const std::string& response) {
    if (!sendToClient(clientSocket, response)) {
        std::cerr << "Failed to send to socket " << clientSocket << std::endl;
    }
}

// Чтение с буферизацией (решает проблему разорванных пакетов)
Chat::ReadResult Chat::readCommand(int socket, std::string& outCommand) {
    char buffer[MESSAGE_LENGTH];
    
    std::string& accumulated = [&]() -> std::string& {
        std::lock_guard<std::mutex> lock(buffersMutex);
        return socketBuffers[socket];
    }();
    
    while (true) {
        size_t pos = accumulated.find('\n');
        if (pos != std::string::npos) {
            outCommand = accumulated.substr(0, pos);
            accumulated.erase(0, pos + 1);
            if (!outCommand.empty() && outCommand.back() == '\r') {
                outCommand.pop_back();
            }
            return ReadResult::SUCCESS;
        }
        
        memset(buffer, 0, sizeof(buffer));
        int bytes_read = recv(socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            accumulated += buffer;
        } 
        else if (bytes_read == 0) {
            return ReadResult::DISCONNECTED;
        } 
        else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return ReadResult::WOULD_BLOCK;
            }
            if (errno == EINTR) continue;
            return ReadResult::ERROR;
        }
    }
}

// ==========================================
// Управление пользователями (Потокобезопасно)
// ==========================================

std::shared_ptr<User> Chat::getUserBySocket(int socket) {
    std::shared_lock<std::shared_mutex> lock(usersMutex);
    for (auto& userPtr : users) {
        if (userPtr->clientSocket == socket) {
            return userPtr;
        }
    }
    return nullptr;
}

std::shared_ptr<User> Chat::getUserByLogin(const std::string& login) {
    std::shared_lock<std::shared_mutex> lock(usersMutex);
    for (auto& userPtr : users) {
        if (userPtr->login == login) {
            return userPtr;
        }
    }
    return nullptr;
}

std::shared_ptr<User> Chat::getUserByName(const std::string& name) {
    std::shared_lock<std::shared_mutex> lock(usersMutex);
    for (auto& userPtr : users) {
        if (userPtr->name == name) {
            return userPtr;
        }
    }
    return nullptr;
}

void Chat::addOrUpdateUser(const User& user) {
    std::lock_guard<std::shared_mutex> lock(usersMutex);
    for(auto& userPtr : users) {
        if(userPtr->login == user.login) {
            *userPtr = user;
            return;
        }
    }
    users.push_back(std::make_shared<User>(user));
}

// Атомарный логин: проверяет и устанавливает сокет за одну операцию
bool Chat::tryLoginUser(const std::string& login, int clientSocket, User& loggedInUser) {
    std::lock_guard<std::shared_mutex> lock(usersMutex);
    for (auto& userPtr : users) {
        if (userPtr->login == login) {
            if (userPtr->clientSocket > 0) return false; // Уже онлайн
            userPtr->clientSocket = clientSocket;
            loggedInUser = *userPtr;
            return true;
        }
    }
    // Если пользователя нет в списке (новый), добавляем
    loggedInUser.clientSocket = clientSocket;
    users.push_back(std::make_shared<User>(loggedInUser));
    return true;
}

void Chat::removeUserBySocket(int socket) {
    std::lock_guard<std::shared_mutex> lock(usersMutex);
    for (auto& userPtr : users) {
        if (userPtr->clientSocket == socket) {
            userPtr->clientSocket = -1; // Просто помечаем как оффлайн
            break; 
        }
    }
}

void Chat::removeUserByLogin(const std::string& login) {
    std::lock_guard<std::shared_mutex> lock(usersMutex);
    users.erase(
        std::remove_if(users.begin(), users.end(),
            [&login](const std::shared_ptr<User>& userPtr) { 
                return userPtr->login == login; 
            }),
        users.end()
    );
}

void Chat::updateUserSocket(const std::string& login, int socket) {
    std::unique_lock<std::shared_mutex> lock(usersMutex);
    for (auto& userPtr : users) {
        if (userPtr->login == login) {
            userPtr->clientSocket = socket;
            break;
        }
    }
}

std::vector<User> Chat::getOnlineUsersCopy() {
    std::shared_lock<std::shared_mutex> lock(usersMutex);
    std::vector<User> onlineUsers;
    for (const auto& userPtr : users) {
        if (userPtr->clientSocket > 0) {
            onlineUsers.push_back(*userPtr);
        }
    }
    return onlineUsers;
}

// ==========================================
// Обработка клиента
// ==========================================

void Chat::cleanupClient(int socket) {
    if (socket <= 0) return;
    
    // Очищаем буфер
    {
        std::lock_guard<std::mutex> lock(buffersMutex);
        socketBuffers.erase(socket);
    }
    
    // Помечаем пользователя оффлайн
    removeUserBySocket(socket);
    
    // Закрываем сокет
    closeSocketSafe(socket);
    
    activeConnections--;
    std::cout << "Client disconnected. Active: " << activeConnections << std::endl;
}

void Chat::handleClient(int socket) {
    bool connected = true;
    while (connected) {
        std::string command;
        ReadResult result = readCommand(socket, command);
        
        switch(result) {
            case ReadResult::SUCCESS:
                if (!processCommand(socket, command)) {
                    connected = false;
                }
                break;
            case ReadResult::DISCONNECTED:
                connected = false;
                break;
            case ReadResult::WOULD_BLOCK:
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            case ReadResult::ERROR:
                std::cerr << "Error reading from socket " << socket << std::endl;
                connected = false;
                break;
        }
    }
    cleanupClient(socket);
}

bool Chat::processCommand(int socket, const std::string& command) {
    if (command.empty()) return true;

    std::istringstream iss(command);
    std::string action;
    iss >> action;

    std::cout << "Received on " << socket << ": " << command << std::endl;

    if (action == "1") { // Регистрация
        std::string login, password, name;
        iss >> login >> password >> name;
        AddNewUser(login, password, name, socket);
    } 
    else if (action == "2") { // Логин
        std::string login, password;
        iss >> login >> password;
        User loggedInUser;
        if (Login(login, password, socket, loggedInUser)) {
            // Ищем указатель на пользователя, чтобы передать в панель
            auto userPtr = getUserByLogin(loggedInUser.login);
            if (userPtr) {
                UserPanel(socket, userPtr);
                return false; // Выход из handleClient, так как UserPanel имеет свой цикл
            }
        }
    } 
    else if (action == "3") { // Все пользователи
        PrintAllUsers(socket);
    } 
    else if (action == "4") { // Публичный чат
        PrintPublicMessage(socket);
    } 
    else if (action == "5") { // Админ
        std::string login, pass;
        iss >> login >> pass;
        if (AdminLogin(login, pass)) {
            sendResponse(socket, "Admin login successful!");
            AdminPanel(socket);
            return false; // Выход из handleClient
        } else {
            sendResponse(socket, "Admin login failed!");
        }
    } 
    else if (action == "7") { // Выход
        return false;
    }
    
    return true;
}

// ==========================================
// Логика приложения (Логин/Регистрация)
// ==========================================

bool Chat::Login(const std::string& login, const std::string& password, int clientSocket, User& loggedInUser) {
    if (dbManager->isUserBanned(login)) {
        sendResponse(clientSocket, "Login failed! Your account has been banned.");
        return false;
    }

    auto authUser = dbManager->authenticateUser(login, password);
    if (!authUser) {
        sendResponse(clientSocket, "Invalid login or password!");
        return false;
    }
    
    loggedInUser = *authUser;
    
    // Атомарно пытаемся залогиниться
    if (!tryLoginUser(login, clientSocket, loggedInUser)) {
        sendResponse(clientSocket, "User already logged in!");
        return false;
    }
    
    sendResponse(clientSocket, "Login successful!");
    return true;
}

void Chat::AddNewUser(const std::string& login, const std::string& password, const std::string& name, int clientSocket) {
    if (getUserByLogin(login)) {
        sendResponse(clientSocket, "Registration failed! Login exists.");
        return;
    }
    
    if (dbManager->registerUser(name, login, password)) {
        sendResponse(clientSocket, "User registered successfully!");
        User newUser;
        newUser.name = name;
        newUser.login = login;
        newUser.clientSocket = -1;
        addOrUpdateUser(newUser);
    } else {
        sendResponse(clientSocket, "Registration failed! DB error.");
    }
}

// ==========================================
// Панели управления (User/Admin)
// ==========================================

void Chat::UserPanel(int clientSocket, std::shared_ptr<User> user) {
    std::cout << "UserPanel started for " << user->name << std::endl;
    bool inSession = true;
    
    while (inSession) {
        std::string command;
        ReadResult result = readCommand(clientSocket, command);
        
        switch(result) {
            case ReadResult::DISCONNECTED: return;
            case ReadResult::ERROR: return;
            case ReadResult::WOULD_BLOCK: 
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            case ReadResult::SUCCESS: break; 
        }
        
        if (command.empty()) continue;

        std::istringstream iss(command);
        std::string action;
        iss >> action;

        if (action == "1") { // Личное сообщение
            std::string recipient, message;
            iss >> recipient;
            std::getline(iss, message);
            if(message.size() > 0) message = message.substr(1); // Убрать пробел
            
            SendMessage(recipient, message, clientSocket, *user);
        }
        else if (action == "2") { // Публичное сообщение
            std::string message;
            std::getline(iss, message);
            if(message.size() > 0) message = message.substr(1);
            
            SendPublicMessage(message, clientSocket, *user);
        }
        else if (action == "3") PrintPrivateMessage(clientSocket, *user);
        else if (action == "4") PrintPublicMessage(clientSocket);
        else if (action == "5") PrintAllUsers(clientSocket);
        else if (action == "6") {
            sendResponse(clientSocket, "Goodbye!");
            updateUserSocket(user->login, -1);
            inSession = false;
        }
    }
}

bool Chat::AdminLogin(const std::string& login, const std::string& password) {
    auto it = adminAkk.find(login);
    return it != adminAkk.end() && it->second == password;
}

void Chat::AdminPanel(int clientSocket) {
    bool inSession = true;
    while (inSession) {
        std::string command;
        ReadResult result = readCommand(clientSocket, command);
        
        switch(result) {
            case ReadResult::DISCONNECTED: return;
            case ReadResult::ERROR: return;
            case ReadResult::WOULD_BLOCK: 
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            case ReadResult::SUCCESS: break; 
        }
        
        if (command.empty()) continue;
        
        std::istringstream iss(command);
        std::string action;
        iss >> action;
        
        if (action == "1") PrintAllUsers(clientSocket);
        else if (action == "admin_2") PrintOnlineUsers(clientSocket);
        else if (action == "admin_3") {
            std::string login; iss >> login;
            banUser(login, clientSocket);
        }
        else if (action == "4") PrintPublicMessage(clientSocket);
        else if (action == "admin_5") {
            std::string login; iss >> login;
            unbanUser(login, clientSocket);
        }
        else if (action == "admin_6") {
            std::string login; iss >> login;
            deleteUser(login, clientSocket);
        }
        else if (action == "admin_7") PrintBannedUsers(clientSocket);
        else if (action == "8") {
            sendResponse(clientSocket, "Exiting admin panel");
            inSession = false;
        }
    }
}

// ==========================================
// Функции сообщений и информации
// ==========================================

void Chat::SendMessage(const std::string& recipientName, const std::string& message, int senderSocket, const User& sender) {
    int recipientId = dbManager->findUserIdByName(recipientName);
    int senderId = dbManager->findUserIdByName(sender.name);
    
    if (recipientId == -1) {
        sendResponse(senderSocket, "Recipient not found!");
        return;
    }

    if (dbManager->savePrivateMessage(senderId, recipientId, message)) {
        sendResponse(senderSocket, "Message sent!");
        
        // Потокобезопасный поиск получателя онлайн
        auto recipientUser = getUserByName(recipientName);
        if (recipientUser && recipientUser->clientSocket > 0) {
            sendResponse(recipientUser->clientSocket, "Private msg from " + sender.name + ": " + message);
        }
    } else {
        sendResponse(senderSocket, "Failed to send message.");
    }
}

void Chat::SendPublicMessage(const std::string& message, int senderSocket, const User& sender) {
    int senderId = dbManager->findUserIdByName(sender.name);
    if (senderId == -1) return;

    if (dbManager->savePublicMessage(senderId, sender.name, message)) {
        std::string broadcast = "Public from " + sender.name + ": " + message;
        
        // Отправка копии списка, чтобы не держать мьютекс во время сетевой отправки
        auto targets = getOnlineUsersCopy();
        int count = 0;
        for(const auto& u : targets) {
            if (u.clientSocket != senderSocket) {
                sendResponse(u.clientSocket, broadcast);
                count++;
            }
        }
        sendResponse(senderSocket, "Sent to " + std::to_string(count) + " users.");
    }
}

void Chat::PrintAllUsers(int clientSocket) {
    auto dbUsers = dbManager->getAllUsers();
    
    // Получаем список онлайн логинов
    auto onlineUsers = getOnlineUsersCopy();
    std::vector<std::string> onlineLogins;
    for(const auto& u : onlineUsers) onlineLogins.push_back(u.login);
    
    std::stringstream ss;
    ss << "--- All Users ---\n";
    for(const auto& u : dbUsers) {
        ss << u.name << " (" << u.login << ")";
        if (std::find(onlineLogins.begin(), onlineLogins.end(), u.login) != onlineLogins.end()) {
            ss << " [ONLINE]";
        }
        ss << "\n";
    }
    sendResponse(clientSocket, ss.str());
}

void Chat::PrintOnlineUsers(int clientSocket) {
    auto onlineUsers = getOnlineUsersCopy();
    std::stringstream ss;
    ss << "--- Online Users ---\n";
    if (onlineUsers.empty()) ss << "No users online.\n";
    for(const auto& u : onlineUsers) {
        ss << u.name << " (" << u.login << ")\n";
    }
    sendResponse(clientSocket, ss.str());
}

void Chat::PrintPublicMessage(int clientSocket) {
    auto msgs = dbManager->getPublicMessages(50);
    std::stringstream ss;
    ss << "--- Public Chat ---\n";
    for(const auto& m : msgs) ss << m.from << ": " << m.text << "\n";
    sendResponse(clientSocket, ss.str());
}

void Chat::PrintPrivateMessage(int clientSocket, const User& user) {
    auto msgs = dbManager->getPrivateMessages(user.login);
    std::stringstream ss;
    ss << "--- Private Chat ---\n";
    for(const auto& m : msgs) {
        ss << (m.from == user.name ? "You -> " : m.from + " -> ") << m.text << "\n";
    }
    sendResponse(clientSocket, ss.str());
}

void Chat::PrintBannedUsers(int clientSocket) {
    auto banned = dbManager->getBannedUsers();
    std::stringstream ss;
    ss << "--- Banned Users ---\n";
    for(const auto& u : banned) ss << u.name << " (" << u.login << ")\n";
    sendResponse(clientSocket, ss.str());
}

// ==========================================
// Админские действия
// ==========================================

void Chat::banUser(const std::string& login, int adminSocket) {
    if (dbManager->banUser(login)) {
        // Кикаем если онлайн
        int targetSocket = -1;
        {
            auto user = getUserByLogin(login);
            if (user && user->clientSocket > 0) targetSocket = user->clientSocket;
        }
        if (targetSocket > 0) {
            sendResponse(targetSocket, "YOU ARE BANNED!");
            cleanupClient(targetSocket);
        }
        sendResponse(adminSocket, "User banned.");
    } else {
        sendResponse(adminSocket, "Ban failed.");
    }
}

void Chat::unbanUser(const std::string& login, int adminSocket) {
    if (dbManager->unbanUser(login)) sendResponse(adminSocket, "User unbanned.");
    else sendResponse(adminSocket, "Unban failed.");
}

void Chat::deleteUser(const std::string& login, int adminSocket) {
    int targetSocket = -1;
    {
        auto user = getUserByLogin(login);
        if (user && user->clientSocket > 0) targetSocket = user->clientSocket;
    }
    
    if (dbManager->deleteUser(login)) {
        if (targetSocket > 0) {
            sendResponse(targetSocket, "ACCOUNT DELETED!");
            cleanupClient(targetSocket);
        }
        removeUserByLogin(login);
        sendResponse(adminSocket, "User deleted.");
    } else {
        sendResponse(adminSocket, "Delete failed.");
    }
}