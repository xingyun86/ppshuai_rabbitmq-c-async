Windows下编译需要注意的问题及解决方案
1.amqp_socket.h
struct amqp_socket_class_t {
  amqp_socket_send_fn send;
  amqp_socket_recv_fn recv;
  amqp_socket_open_fn open;
  amqp_socket_close_fn close;
  amqp_socket_get_sockfd_fn get_sockfd;
+++amqp_socket_delete_fn delete_;
---amqp_socket_delete_fn delete_;
};
2.amqp_socket.cpp
void amqp_socket_delete(amqp_socket_t *self) {
  if (self) {
    +assert(self->klass->delete_);
    +self->klass->delete_ (self);
    -assert(self->klass->delete);
    -self->klass->delete (self);
  }
}