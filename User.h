#pragma once
#include <vector>
#include <iostream>
#include <string>
#include "Messages.h"
#include <sstream>
#include <limits>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

class User {
public:
    std::string name, login, password, serverAddress;
    std::vector<Message> privateMessages; 
    int clientSocket = -1, id;
    bool ban = false;

    User() {} 
    
    User(int userId, const std::string& userName, const std::string& userLogin) :
        id(userId),
        name(userName),
        login(userLogin)
    {}
    
    ~User() {
        if (clientSocket != -1) {
            close(clientSocket);
            std::cout << "Client socket closed for user " << name << std::endl;
        }
    }
};