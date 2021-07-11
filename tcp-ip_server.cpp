#include <iostream>
#include <cstdio>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <strings.h>
#include <string.h>
#include <wait.h> 
#include <sstream>
#include <queue>

#define BUFFER_SIZE 100
#define TIMEOUT 1
#define TIMEOUT_RECHARGING 5
#define CONNECTION_LIMIT 10
#define END_SEQ "\a\b"

#define SERVER_MOVE "102 MOVE"
#define SERVER_TURN_LEFT "103 TURN LEFT"
#define SERVER_TURN_RIGHT "104 TURN RIGHT"
#define SERVER_PICK_UP "105 GET MESSAGE"
#define SERVER_LOGOUT "106 LOGOUT"
#define SERVER_OK "200 OK"
#define SERVER_LOGIN_FAILED "300 LOGIN FAILED"
#define SERVER_SYNTAX_ERROR "301 SYNTAX ERROR"
#define SERVER_LOGIC_ERROR "302 LOGIC ERROR"

#define CLIENT_RECHARGING "RECHARGING"
#define CLIENT_FULL_POWER "FULL POWER"
#define CLIENT_USERNAME_MAXLEN 12
#define CLIENT_CONFIRMATION_MAXLEN 7
#define CLIENT_OK_MAXLEN 12
#define CLIENT_RECHARGING_MAXLEN 12
#define CLIENT_FULL_POWER_MAXLEN 12
#define CLIENT_MESSAGE_MAXLEN 100

#define SERVER_PORT 3999
#define SERVER_KEY 54621
#define CLIENT_KEY 45328
#define HASH_MOD 65536

#define UP 0
#define RIGHT 1
#define DOWN 2
#define LEFT 3

using namespace std;

class Gadgets {
  public:
    // function to validate
    static bool isInteger(const string & token) {
      for(size_t i = 0; i < token.length(); i++) {
        if(i == 0) {
          if(token[i] == '+' || token[i] == '-')
            continue;
        }
        if(!isdigit(token[i]))
          return false;
      }
      return true;
    }
};

class Connection {
  public:
    Connection() {}

    Connection(int client) {
      this->client = client;
      buffer.clear();
    }
    
    // add END_SEQ and send message to client
    void sendData(string message) {
      message += END_SEQ;
      send(client, message.data(), message.length(), MSG_NOSIGNAL);
    }

    // receive data in variable message if message is ok, return true and message as first parameter
    bool recvData(string & message, size_t limit) {
      if(!getNextMessage(message, limit))
        return false;
      return true;  
    }
 
  private:
    int client;
    string buffer;
    deque<string> messages;

    // read received packet and push chars in buffer, check timeout using select
    bool readSocket() {
      char cbuffer[BUFFER_SIZE + 16];
      struct timeval timeout;
      timeout.tv_sec = TIMEOUT;
      timeout.tv_usec = 0;
      fd_set sockets;
      FD_ZERO(&sockets);
      FD_SET(client, &sockets);
      select(client + 1, &sockets, NULL, NULL, &timeout);
      if(!FD_ISSET(client, &sockets)) 
        return false;
      int bytesRead = recv(client, cbuffer, BUFFER_SIZE, 0);
      for(int i = 0; i < bytesRead; i++)
        buffer.push_back(cbuffer[i]);
      return true;
    }

    // get message length if message is complete, if END_SEQ is missing, return 0
    size_t getMessageLength() {
      for(size_t i = 1; i < buffer.length(); i++) 
        if(buffer[i - 1] == '\a' && buffer[i] == '\b')
          return i + 1;
      return 0;
    }

    // get next message from deque of complete messages
    bool getNextMessage(string & msg, size_t limit) {
      if(messages.empty())
        if(!pushNewMessage(limit))
          return false;
      msg = messages.front();
      messages.pop_front();
      
      if(msg.substr(0, msg.length() - strlen(END_SEQ)) == CLIENT_RECHARGING) {
        if(!recharge()) 
          return false;
        if(messages.empty())
          if(!pushNewMessage(limit))
            return false;
        msg = messages.front();
        messages.pop_front();
        if (msg.substr(0, msg.length() - strlen(END_SEQ)) != CLIENT_FULL_POWER) {
          sendData(SERVER_LOGIC_ERROR);
          return false;
        }
        if(messages.empty())
          if(!pushNewMessage(limit))
            return false;
        msg = messages.front();
        messages.pop_front();
      }
    
      if(msg.length() > limit) {
        sendData(SERVER_SYNTAX_ERROR);
        return false;
      }

      msg = msg.substr(0, msg.length() - strlen(END_SEQ));
      return true;
    }

    // robot is recharging, function checks time for timeout
    bool recharge() {
      string message;
      struct timeval timeout;
      timeout.tv_sec = TIMEOUT_RECHARGING;
      timeout.tv_usec = 0;
      fd_set sockets;
      FD_ZERO(&sockets);
      FD_SET(client, &sockets);
      select(client + 1, &sockets, NULL, NULL, &timeout);
      if(!FD_ISSET(client, &sockets))
        return false;
      return true;
    }

    // of there is complete usable message in buffer, push it to deque of messages
    bool pushNewMessage(size_t limit) {
      size_t messageLen = getMessageLength();
      while(messageLen == 0) {
        if(buffer.length() > limit - 1) {
          sendData(SERVER_SYNTAX_ERROR);
          return false;
        }
        if(!readSocket())
          return false;
        if(buffer.length() == 0)
          return false;
        messageLen = getMessageLength();
      }
      string message = buffer.substr(0, messageLen);
      messages.push_back(message);
      buffer = buffer.substr(messageLen);
      return true;
    }
};

class Robot {
  public:
    Robot(const Connection & conn) {
      this->conn = conn;
    }

    // sending hash codes and confirmations between robot and server
    bool auth() {
      string message;
      int nameSum, hash;

      if(!conn.recvData(message, CLIENT_USERNAME_MAXLEN))
        return false;

      nameSum = 0;
      for(size_t i = 0; i < message.length(); i++)
        nameSum += (int)message[i]; 

      hash = ( ( ( nameSum * 1000 ) % HASH_MOD ) + SERVER_KEY ) % HASH_MOD;
      conn.sendData(to_string(hash));

      if(!conn.recvData(message, CLIENT_CONFIRMATION_MAXLEN))
        return false;

      if(!Gadgets::isInteger(message)) {
        conn.sendData(SERVER_SYNTAX_ERROR);
        return false;
      }

      hash = ( ( ( nameSum * 1000 ) % HASH_MOD ) + CLIENT_KEY ) % HASH_MOD;
      int receivedHash = stoi(message);
      if(receivedHash != hash) {
        conn.sendData(SERVER_LOGIN_FAILED);
        return false;
      }
      conn.sendData(SERVER_OK);
      cout << "Authetication successful\n"; 
      return true;
    }

    // go to the center of the map
    bool goToArea() {
      if(!initPos())
        return false;
      while(x > 0)
        if(!move(LEFT))
          return false;
      while(x < 0)
        if(!move(RIGHT))
          return false;
      while(y > 0)
        if(!move(DOWN))
          return false;
      while(y < 0)
        if(!move(UP))
          return false;
      return true;
    }

    // start searching in area, moving is snake-like from center
    bool startSearching() {
      printf("The process of searching begins\n");
      int dir = UP;
      int i = 0, j = 1;
      while(1) {
        if(!pickUpMessage(dir, j)) 
          return false;
        dir++;
        if(dir == 4)
          dir = 0;
        if(i % 2)
          j++;
        i++;
        if(x > 2 || x < -2 || y > 2 || y < -2)
          break;
      }
      return true;
    }

  private:
    Connection conn;
    int x;
    int y;
    int direction;

    // move robot in requested direction "dir"
    bool move(int dir) {
      if(!turn(dir))
        return false;
      int oldX, oldY;
      oldX = x;
      oldY = y;
      if(!moveForward())
        return false;
      while(isStuck(oldX, oldY))
        if(!moveForward())
          return false;
      return true;
    }

    // move robot forward and get coordinates from CLIENT_OK
    bool moveForward() {
      string message, token;
      conn.sendData(SERVER_MOVE);
      if(!conn.recvData(message, CLIENT_OK_MAXLEN))
        return false;
      message = message.substr(3);
      istringstream iss (message);
      
      token = message.substr(0, message.find(" "));
      if(!Gadgets::isInteger(token)) {
        conn.sendData(SERVER_SYNTAX_ERROR);
        return false;
      }
      try {
        x = stoi(token);
      } catch (...) {
        conn.sendData(SERVER_SYNTAX_ERROR);
        return false;
      }
      
      token = message.substr(message.find(" ") + 1);
      if(!Gadgets::isInteger(token)) {
        conn.sendData(SERVER_SYNTAX_ERROR);
        return false;
      }
      try {
        y = stoi(token);
      } catch (...) {
        conn.sendData(SERVER_SYNTAX_ERROR);
        return false;
      }
      return true;
    }

    //turn robot in direction "dir"
    bool turn(int dir) {
      while(direction != dir) {
        string tmp;
        conn.sendData(SERVER_TURN_RIGHT);
        if(!conn.recvData(tmp, CLIENT_OK_MAXLEN))
          return false;
        direction++;
        if(direction == 4)
          direction = 0;
      }
      return true;
    }

    // check if robot did not move
    bool isStuck(const int oldX, const int oldY) {
      if(oldX == x && oldY == y)
        return true;
      return false;
    }

    // move forward twice to get position and direction of robot
    bool initPos() {
      string message;
      stringstream ss;
      int oldX, oldY;
      if(!moveForward())
        return false;
      oldX = x;
      oldY = y;
      if(!moveForward())
        return false;
      while(isStuck(oldX, oldY))
        if(!moveForward())
          return false;

      if(oldX < x) 
        direction = RIGHT;
      else if(oldX > x)
        direction = LEFT;
      else if(oldY < y)
        direction = UP;
      else if(oldY > y)
        direction = DOWN;

      return true;
    }

    // try to pick up a message, return true to continue with searching,
    // return false if message was found or something went wrong
    int pickUpMessage(int dir, int stepCnt) { 
      string message;
      for(int i = 0; i < stepCnt; i++) {
        conn.sendData(SERVER_PICK_UP);
        if(!conn.recvData(message, CLIENT_MESSAGE_MAXLEN))
          return false;
        if(foundSecret(message))
          return false;
        if(!move(dir))
          return false;
      }
      return true;
    }

    bool foundSecret(const string & message) { // if secret message is found, send LOGOUT message and return true, if field was empty(message length is 0), return false
      if(message.length() == 0)
        return false;
      conn.sendData(SERVER_LOGOUT);
      cout << "Message found ... " << message << "\n";
      return true;
    }
};

bool run(int client) {
  Connection conn (client);
  Robot robot (conn);
  if(!robot.auth()) 
    return false;
  if(!robot.goToArea())
    return false;
  if(!robot.startSearching()) 
    return false;
  return true;
}

int main() {
  int listening_socket = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  bzero(&addr, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(SERVER_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  bind(listening_socket, (struct sockaddr*) &addr, sizeof(addr));
  listen(listening_socket, CONNECTION_LIMIT);
  struct sockaddr_in client_addr;
  socklen_t client_addr_length;

  while(1) {
    int client = accept(listening_socket, (struct sockaddr*) &client_addr, &client_addr_length); // accept client
    pid_t pid = fork(); // fork new process
    if(pid == 0) {
      printf("----- CLIENT ACCEPTED -----\n");
      close(listening_socket);
      run(client);
      close(client);
      printf("------ CLIENT CLOSED ------\n");
      printf("________________________________________\n");
      return 0;
    }
    int status = 0;
    waitpid(0, &status, WNOHANG); // no zombies
    close(client);
  }
  close(listening_socket);
  return 0;
}