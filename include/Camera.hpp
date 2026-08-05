#ifndef CAMERA_H
#define CAMERA_H

#include <glad/glad.h>
#include <Math/Constants.hpp>
#include <Math/Vector3.hpp>
#include <Math/Matrix4.hpp>

enum Camera_Movement {
    FORWARD,
    BACKWARD,
    LEFT,
    RIGHT,
    UPWARD,
    DOWNWARD
};

const float YAW = -90.0f;
const float PITCH = 0.0f;
const float SPEED = 2.5f;
const float SENSITIVITY = 0.1f;
const float ZOOM = 45.0f;

class Camera {
public:
    Math::Vector3 Position;
    Math::Vector3 Front;
    Math::Vector3 Up;
    Math::Vector3 Right;
    Math::Vector3 WorldUp;

    float Yaw;
    float Pitch;
    float MovementSpeed;
    float MouseSensitivity;
    float Zoom;

    Camera(Math::Vector3 position = Math::Vector3(), Math::Vector3 up = Math::Vector3(0.0f, 1.0f, 0.0f), float yaw = YAW, float pitch = PITCH) : Front(Math::Vector3(0.0f, 0.0f, -1.0f)), MovementSpeed(SPEED), MouseSensitivity(SENSITIVITY), Zoom(ZOOM) {
        Position = position;
        WorldUp = up;
        Yaw = yaw;
        Pitch = pitch;
        updateCameraVectors();
    }

    Camera(float posX, float posY, float posZ, float upX, float upY, float upZ, float yaw, float pitch) : Front(Math::Vector3(0.0f, 0.0f, -1.0f)), MovementSpeed(SPEED), MouseSensitivity(SENSITIVITY), Zoom(ZOOM)
    {
        Position = Math::Vector3(posX, posY, posZ);
        WorldUp = Math::Vector3(upX, upY, upZ);
        Yaw = yaw;
        Pitch = pitch;
        updateCameraVectors();
    }

    Math::Matrix4 GetViewMatrix() {
        return Math::Matrix4::LookAt(Position, Position + Front, Up);
    }

    void ProcessKeyboard(Camera_Movement direction, float deltaTime) {
        float velocity = MovementSpeed * deltaTime;
        if (direction == FORWARD)
            Position += Front * velocity;
        if (direction == BACKWARD)
            Position -= Front * velocity;
        if (direction == LEFT)
            Position -= Right * velocity;
        if (direction == RIGHT)
            Position += Right * velocity;
        if (direction == UPWARD)
            Position += Up * velocity;
        if (direction == DOWNWARD)
            Position -= Up * velocity;
    }

    void ProcessMouseMovement(float xoffset, float yoffset, GLboolean constrainPitch = true) {
        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;

        Yaw += xoffset;
        Pitch += yoffset;

        if (constrainPitch)
        {
            if (Pitch > 89.0f)
                Pitch = 89.0f;
            if (Pitch < -89.0f)
                Pitch = -89.0f;
        }

        updateCameraVectors();
    }

    void ProcessMouseScroll(float yoffset) {
        Zoom -= (float)yoffset;
        if (Zoom < 1.0f)
            Zoom = 1.0f;
        if (Zoom > 45.0f)
            Zoom = 45.0f;
    }

private:
    void updateCameraVectors() {
        Math::Vector3 front;
        front.x = cos(Math::radians(Yaw)) * cos(Math::radians(Pitch));
        front.y = sin(Math::radians(Pitch));
        front.z = sin(Math::radians(Yaw)) * cos(Math::radians(Pitch));
        Front = front.normalize();
        Right = Front.cross(WorldUp).normalize();
        Up = Right.cross(Front).normalize();
    }    
};

#endif