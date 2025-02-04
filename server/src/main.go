package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"time"

	"github.com/gorilla/websocket"
)

// 声明全局的 Hub 实例
var hub *Hub

// tasksHandler 处理 /tasks 路由请求，并将主机A的信息发送给所有 WebSocket 客户端
func tasksHandler(w http.ResponseWriter, r *http.Request) {
	// 解析主机A的 IP 和端口
	ip, port, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		http.Error(w, "无法解析客户端地址", http.StatusInternalServerError)
		return
	}
	log.Printf("Request /tasks has been processed from IP: %s, Port: %s", ip, port)

	// 提取 URL 查询参数
	addressParam := r.URL.Query().Get("address")
	modelParam := r.URL.Query().Get("model")
	versionParam := r.URL.Query().Get("version")

	// 构造具体转发数据部分，放置在 data 字段内
	data := map[string]string{
		"address": addressParam,
		"model":   modelParam,
		"version": versionParam,
	}

	// 构造最终的 JSON 消息格式：
	// protocol_id 和客户端相关信息放在顶层，数据具体内容封装在 data 字段内
	messageWrapper := map[string]interface{}{
		"protocol_id": 1,
		"data":        data,
	}
	jsonMsg, err := json.Marshal(messageWrapper)
	if err != nil {
		log.Printf("JSON marshaling error: %v", err)
		http.Error(w, "Internal Server Error", http.StatusInternalServerError)
		return
	}

	// 将消息通过 hub 的 broadcast 通道发送给所有在线的 WebSocket 客户端
	hub.broadcast <- jsonMsg

	// 同时返回响应给发起请求的 HTTP 客户端
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	fmt.Fprintln(w, "Request /tasks processed and info broadcasted to websocket clients.")
}

func settingHandler(w http.ResponseWriter, r *http.Request) {
	ip, port, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		http.Error(w, "无法解析客户端地址", http.StatusInternalServerError)
		return
	}
	log.Printf("Request /setting has been processed from IP: %s, Port: %s", ip, port)

	fmt.Fprintln(w, "Request /setting has been processed:", r.Host)
}

// 常量定义
const (
	// 写操作超时
	writeWait = 10 * time.Second
	// 读操作超时（用于 Pong 响应）
	pongWait = 60 * time.Second
	// Ping 周期
	pingPeriod = (pongWait * 9) / 10
	// 允许的最大消息长度
	maxMessageSize = 1024
)

// 将 HTTP 连接升级为 WebSocket 连接的 Upgrader 配置
var upgrader = websocket.Upgrader{
	ReadBufferSize:  1024,
	WriteBufferSize: 1024,
	// 允许所有来源（测试时可用，生产环境需要严格控制）
	CheckOrigin: func(r *http.Request) bool {
		return true
	},
}

// Hub 管理所有连接的客户端
type Hub struct {
	// 当前所有活跃的客户端
	clients map[*Client]bool
	// 广播通道，用于转发消息
	broadcast chan []byte
	// 客户端注册请求
	register chan *Client
	// 客户端注销请求
	unregister chan *Client
}

// newHub 创建一个新的 Hub 实例
func newHub() *Hub {
	return &Hub{
		clients:    make(map[*Client]bool),
		broadcast:  make(chan []byte),
		register:   make(chan *Client),
		unregister: make(chan *Client),
	}
}

// run 启动 Hub 循环，处理注册、注销和消息广播
func (h *Hub) run() {
	for {
		select {
		case client := <-h.register:
			h.clients[client] = true
			log.Printf("Client registered: %s", client.id)
		case client := <-h.unregister:
			if _, ok := h.clients[client]; ok {
				delete(h.clients, client)
				close(client.send)
				log.Printf("Client unregistered: %s", client.id)
			}
		case message := <-h.broadcast:
			// 将消息广播给所有已注册的客户端
			for client := range h.clients {
				select {
				case client.send <- message:
				default:
					close(client.send)
					delete(h.clients, client)
				}
			}
		}
	}
}

// Client 表示一个 WebSocket 连接
type Client struct {
	hub  *Hub
	conn *websocket.Conn
	// 用于发送消息的缓冲通道
	send chan []byte
	// 客户端标识，使用其远程地址
	id string
}

// readPump 负责从客户端连接不断读取消息，并按照协议格式处理
func (c *Client) readPump() {
	defer func() {
		// 发生异常或退出时注销该客户端，并关闭连接
		c.hub.unregister <- c
		c.conn.Close()
	}()

	// 限制收到的消息大小，设置读超时、心跳检测处理
	c.conn.SetReadLimit(maxMessageSize)
	c.conn.SetReadDeadline(time.Now().Add(pongWait))
	c.conn.SetPongHandler(func(string) error {
		c.conn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})

	for {
		_, message, err := c.conn.ReadMessage()
		if err != nil {
			// 如果非正常关闭则打日志
			if websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseAbnormalClosure) {
				log.Printf("Unexpected close error from %s: %v", c.id, err)
			}
			break
		}

		// 尝试解析接收到的 JSON 数据，要求格式如下：
		// {
		//    "protocol_id": number,
		//    "data": { ... }
		// }
		var msgData map[string]interface{}
		if err := json.Unmarshal(message, &msgData); err != nil {
			log.Printf("Error parsing JSON message from %s: %v", c.id, err)
			continue
		}

		// 检查是否包含 protocol_id 字段
		protocol, ok := msgData["protocol_id"]
		if !ok {
			log.Printf("Received message missing protocol_id from %s", c.id)
			continue
		}
		// 由于 JSON 数字默认解析为 float64
		protocolID, ok := protocol.(float64)
		if !ok {
			log.Printf("Invalid protocol_id type in message from %s", c.id)
			continue
		}

		// 检查是否包含 data 字段
		dataField, ok := msgData["data"]
		if !ok {
			log.Printf("Received message missing data field from %s", c.id)
			continue
		}
		data := make(map[string]interface{})
		data["msg"] = dataField.(string) + " # Review Finished"
		// 根据 protocol_id 选择处理方式
		switch int(protocolID) {
		case 1:
			// 对于 protocol_id = 1，采用 ECHO 功能：
			// 将收到的 data 重新封装成相同的 JSON 格式回复给客户端
			response := map[string]interface{}{ // 回复客户端的2号协议
				"protocol_id": 2,
				"data":        data,
			}
			responseJSON, err := json.Marshal(response)
			if err != nil {
				log.Printf("Error encoding echo response for %s: %v", c.id, err)
				continue
			}
			log.Printf("Echoing message to %s: %s", c.id, responseJSON)
			// 将回复消息写入客户端的发送 channel，由 writePump 负责实际调用系统网络接口发送数据
			c.send <- responseJSON

		default:
			log.Printf("Unsupported protocol_id %v from %s", protocolID, c.id)
		}
	}
}

// writePump 负责从 send 通道中读取消息并写回客户端
func (c *Client) writePump() {
	ticker := time.NewTicker(pingPeriod)
	defer func() {
		ticker.Stop()
		c.conn.Close()
	}()
	for {
		select {
		case message, ok := <-c.send:
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if !ok {
				// send 通道关闭，发送关闭消息
				c.conn.WriteMessage(websocket.CloseMessage, []byte{})
				return
			}
			// 获取写入器
			w, err := c.conn.NextWriter(websocket.TextMessage)
			if err != nil {
				return
			}
			w.Write(message)

			// 如果有排队的消息，一并写入
			n := len(c.send)
			for i := 0; i < n; i++ {
				w.Write([]byte{'\n'})
				w.Write(<-c.send)
			}

			if err := w.Close(); err != nil {
				return
			}
		case <-ticker.C:
			// 定时发送 ping 以维持连接
			c.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := c.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		}
	}
}

// serveWs 将 HTTP 连接升级为 WebSocket 连接，并注册到 Hub 中
func serveWs(hub *Hub, w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("Upgrade error: %v", err)
		return
	}
	client := &Client{
		hub:  hub,
		conn: conn,
		send: make(chan []byte, 256),
		id:   conn.RemoteAddr().String(),
	}
	client.hub.register <- client

	// 分别启动读写 goroutine
	go client.writePump()
	go client.readPump()
}

func main() {
	// 初始化并启动 Hub 循环（这里使用全局 hub 变量）
	hub = newHub()
	go hub.run()

	// 注册 RESTful API 路由
	http.HandleFunc("/tasks", tasksHandler)
	http.HandleFunc("/setting", settingHandler)

	// 注册 WebSocket 路由（所有 WebSocket 客户端通过 "/ws" 路径接入）
	http.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		serveWs(hub, w, r)
	})

	// 统一监听单个端口，例如 8194
	addr := ":8194"
	log.Printf("服务器启动，监听端口 %s", addr)
	if err := http.ListenAndServe(addr, nil); err != nil {
		log.Fatalf("ListenAndServe error: %v", err)
	}
}
