#include "vr_input.h"

//#if __ANDROID__

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/keycodes.h"
#include "../client/client.h"
#include "vr_base.h"
#include "../VrApi/Include/VrApi_Input.h"
#include "../VrApi/Include/VrApi_Helpers.h"
#include "vr_clientinfo.h"

#include <unistd.h>
#include <jni.h>

#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

enum {
	VR_TOUCH_AXIS_UP = 1 << 0,
	VR_TOUCH_AXIS_DOWN = 1 << 1,
	VR_TOUCH_AXIS_LEFT = 1 << 2,
	VR_TOUCH_AXIS_RIGHT = 1 << 3,
	VR_TOUCH_AXIS_TRIGGER_INDEX = 1 << 4,
};

typedef struct {
	uint32_t buttons;
	uint32_t axisButtons;
} vrController_t;

vr_clientinfo_t vr;

static qboolean controllerInit = qfalse;

static vrController_t leftController;
static vrController_t rightController;
static int in_vrEventTime = 0;
static double lastframetime = 0;

static float pressedThreshold = 0.75f;
static float releasedThreshold = 0.5f;

extern cvar_t *cl_sensitivity;
extern cvar_t *m_pitch;
extern cvar_t *m_yaw;

float radians(float deg) {
    return (deg * M_PI) / 180.0;
}

float degrees(float rad) {
    return (rad * 180.0) / M_PI;
}


#ifndef EPSILON
#define EPSILON 0.001f
#endif

extern cvar_t *vr_righthanded;
extern cvar_t *vr_snapturn;
extern cvar_t *vr_extralatencymode;
extern cvar_t *vr_directionMode;
extern cvar_t *vr_weaponPitch;
extern cvar_t *vr_heightAdjust;
extern cvar_t *vr_twoHandedWeapons;
extern cvar_t *vr_refreshrate;
extern cvar_t *vr_weaponScope;

jclass callbackClass;
jmethodID android_haptic_event;

qboolean alt_key_mode_active = qfalse;

void rotateAboutOrigin(float x, float y, float rotation, vec2_t out)
{
	out[0] = cosf(DEG2RAD(-rotation)) * x  +  sinf(DEG2RAD(-rotation)) * y;
	out[1] = cosf(DEG2RAD(-rotation)) * y  -  sinf(DEG2RAD(-rotation)) * x;
}

static ovrVector3f normalizeVec(ovrVector3f vec) {
    //NOTE: leave w-component untouched
    //@@const float EPSILON = 0.000001f;
    float xxyyzz = vec.x*vec.x + vec.y*vec.y + vec.z*vec.z;
    //@@if(xxyyzz < EPSILON)
    //@@    return *this; // do nothing if it is zero vector

    //float invLength = invSqrt(xxyyzz);
    ovrVector3f result;
    float invLength = 1.0f / sqrtf(xxyyzz);
    result.x = vec.x * invLength;
    result.y = vec.y * invLength;
    result.z = vec.z * invLength;
    return result;
}

void NormalizeAngles(vec3_t angles)
{
    while (angles[0] >= 90) angles[0] -= 180;
    while (angles[1] >= 180) angles[1] -= 360;
    while (angles[2] >= 180) angles[2] -= 360;
    while (angles[0] < -90) angles[0] += 180;
    while (angles[1] < -180) angles[1] += 360;
    while (angles[2] < -180) angles[2] += 360;
}

void GetAnglesFromVectors(const ovrVector3f forward, const ovrVector3f right, const ovrVector3f up, vec3_t angles)
{
    float sr, sp, sy, cr, cp, cy;

    sp = -forward.z;

    float cp_x_cy = forward.x;
    float cp_x_sy = forward.y;
    float cp_x_sr = -right.z;
    float cp_x_cr = up.z;

    float yaw = atan2(cp_x_sy, cp_x_cy);
    float roll = atan2(cp_x_sr, cp_x_cr);

    cy = cos(yaw);
    sy = sin(yaw);
    cr = cos(roll);
    sr = sin(roll);

    if (fabs(cy) > EPSILON)
    {
        cp = cp_x_cy / cy;
    }
    else if (fabs(sy) > EPSILON)
    {
        cp = cp_x_sy / sy;
    }
    else if (fabs(sr) > EPSILON)
    {
        cp = cp_x_sr / sr;
    }
    else if (fabs(cr) > EPSILON)
    {
        cp = cp_x_cr / cr;
    }
    else
    {
        cp = cos(asin(sp));
    }

    float pitch = atan2(sp, cp);

    angles[0] = pitch / (M_PI*2.f / 360.f);
    angles[1] = yaw / (M_PI*2.f / 360.f);
    angles[2] = roll / (M_PI*2.f / 360.f);

    NormalizeAngles(angles);
}

void QuatToYawPitchRoll(ovrQuatf q, vec3_t rotation, vec3_t out) {

    ovrMatrix4f mat = ovrMatrix4f_CreateFromQuaternion( &q );

    if (rotation[0] != 0.0f || rotation[1] != 0.0f || rotation[2] != 0.0f)
    {
        ovrMatrix4f rot = ovrMatrix4f_CreateRotation(radians(rotation[0]), radians(rotation[1]), radians(rotation[2]));
        mat = ovrMatrix4f_Multiply(&mat, &rot);
    }

    ovrVector4f v1 = {0, 0, -1, 0};
    ovrVector4f v2 = {1, 0, 0, 0};
    ovrVector4f v3 = {0, 1, 0, 0};

    ovrVector4f forwardInVRSpace = ovrVector4f_MultiplyMatrix4f(&mat, &v1);
    ovrVector4f rightInVRSpace = ovrVector4f_MultiplyMatrix4f(&mat, &v2);
    ovrVector4f upInVRSpace = ovrVector4f_MultiplyMatrix4f(&mat, &v3);

    ovrVector3f forward = {-forwardInVRSpace.z, -forwardInVRSpace.x, forwardInVRSpace.y};
    ovrVector3f right = {-rightInVRSpace.z, -rightInVRSpace.x, rightInVRSpace.y};
    ovrVector3f up = {-upInVRSpace.z, -upInVRSpace.x, upInVRSpace.y};

    ovrVector3f forwardNormal = normalizeVec(forward);
    ovrVector3f rightNormal = normalizeVec(right);
    ovrVector3f upNormal = normalizeVec(up);

    GetAnglesFromVectors(forwardNormal, rightNormal, upNormal, out);
}

static void IN_SendButtonAction(const char* action, qboolean pressed)
{
    if (action)
    {
        //handle our special actions first
        if (strcmp(action, "+alt") == 0)
        {
            alt_key_mode_active = pressed;
        }
        else if (strcmp(action, "+weapon_stabilise") == 0)
        {
            vr.weapon_stabilised = pressed;
        }
        else if (action[0] == '+')
        {
            char command[256];
            Com_sprintf(command, sizeof(command), "%s%s\n", pressed ? "+" : "-", action + 1);
            Cbuf_AddText(command);
        }
        else if (pressed)
        {
            char command[256];
            Com_sprintf(command, sizeof(command), "%s\n", action);
            Cbuf_AddText(command);
        }
    }
}

void VR_HapticEvent(const char* event, int position, int flags, int intensity, float angle, float yHeight )
{
    engine_t* engine = VR_GetEngine();
    jstring StringArg1 = (*(engine->java.Env))->NewStringUTF(engine->java.Env, event);
    (*(engine->java.Env))->CallVoidMethod(engine->java.Env, engine->java.ActivityObject, android_haptic_event, StringArg1, position, flags, intensity, angle, yHeight);
}

static qboolean IN_GetButtonAction(const char* button, char* action)
{
    char cvarname[256];
    Com_sprintf(cvarname, 256, "vr_button_map_%s%s", button, alt_key_mode_active ? "_ALT" : "");
    char * val = Cvar_VariableString(cvarname);
    if (val && strlen(val) > 0)
    {
        Com_sprintf(action, 256, "%s", val);
        return qtrue;
    }

    //If we didn't find something for this button and the alt key is active, then see if the un-alt key has a function
    if (alt_key_mode_active)
    {
        Com_sprintf(cvarname, 256, "vr_button_map_%s", button);
        char * val = Cvar_VariableString(cvarname);
        if (val && strlen(val) > 0)
        {
            Com_sprintf(action, 256, "%s", val);
            return qtrue;
        }
    }

    return qfalse;
}

static float length(float x, float y)
{
    return sqrtf(powf(x, 2.0f) + powf(y, 2.0f));
}

void IN_VRInit( void )
{
	memset(&vr, 0, sizeof(vr));

	engine_t *engine = VR_GetEngine();
    callbackClass = (*engine->java.Env)->GetObjectClass(engine->java.Env, engine->java.ActivityObject);
    android_haptic_event = (*engine->java.Env)->GetMethodID(engine->java.Env, callbackClass, "haptic_event","(Ljava/lang/String;IIIFF)V");
}

static void IN_VRController( qboolean isRightController, ovrTracking remoteTracking )
{
    if (isRightController == (vr_righthanded->integer != 0))
	{
		//Set gun angles - We need to calculate all those we might need (including adjustments) for the client to then take its pick
		vec3_t rotation = {0};
		rotation[PITCH] = vr_weaponPitch->value;
		QuatToYawPitchRoll(remoteTracking.HeadPose.Pose.Orientation, rotation, vr.weaponangles);

		VectorSubtract(vr.weaponangles_last, vr.weaponangles, vr.weaponangles_delta);
		VectorCopy(vr.weaponangles, vr.weaponangles_last);

		///Weapon location relative to view
		vr.weaponposition[0] = remoteTracking.HeadPose.Pose.Position.x;
		vr.weaponposition[1] = remoteTracking.HeadPose.Pose.Position.y + vr_heightAdjust->value;
		vr.weaponposition[2] = remoteTracking.HeadPose.Pose.Position.z;

		VectorCopy(vr.weaponoffset_last[1], vr.weaponoffset_last[0]);
		VectorCopy(vr.weaponoffset, vr.weaponoffset_last[1]);
		VectorSubtract(vr.weaponposition, vr.hmdposition, vr.weaponoffset);

		if (vr.virtual_screen ||
            cl.snap.ps.pm_type == PM_INTERMISSION)
        {
		    int mouse_multiplier = 10;
            Com_QueueEvent(in_vrEventTime, SE_MOUSE, vr.weaponangles_delta[YAW] * mouse_multiplier, -vr.weaponangles_delta[PITCH] * mouse_multiplier, 0, NULL);
        }
	} else {
        vec3_t rotation = {0};
        rotation[PITCH] = vr_weaponPitch->value;
        QuatToYawPitchRoll(remoteTracking.HeadPose.Pose.Orientation, rotation, vr.offhandangles);

        ///location relative to view
        vr.offhandposition[0] = remoteTracking.HeadPose.Pose.Position.x;
        vr.offhandposition[1] = remoteTracking.HeadPose.Pose.Position.y + vr_heightAdjust->value;
        vr.offhandposition[2] = remoteTracking.HeadPose.Pose.Position.z;

        VectorCopy(vr.offhandoffset_last[1], vr.offhandoffset_last[0]);
        VectorCopy(vr.offhandoffset, vr.offhandoffset_last[1]);
        VectorSubtract(vr.offhandposition, vr.hmdposition, vr.offhandoffset);
	}

    vr.weapon_zoomed = vr_weaponScope->integer &&
                       vr.weapon_stabilised &&
                       (cl.snap.ps.weapon == WP_RAILGUN) &&
                       (VectorLength(vr.weaponoffset) < 0.24f) &&
                       cl.snap.ps.stats[STAT_HEALTH] > 0;

    if (vr_twoHandedWeapons->integer && vr.weapon_stabilised)
    {
        //Apply smoothing to the weapon hand
        vec3_t smooth_weaponoffset;
        VectorAdd(vr.weaponoffset, vr.weaponoffset_last[0], smooth_weaponoffset);
        VectorAdd(smooth_weaponoffset, vr.weaponoffset_last[1],smooth_weaponoffset);
        VectorScale(smooth_weaponoffset, 1.0f/3.0f, smooth_weaponoffset);

        vec3_t vec;
        VectorSubtract(vr.offhandoffset, smooth_weaponoffset, vec);

        float zxDist = length(vec[0], vec[2]);

        if (zxDist != 0.0f && vec[2] != 0.0f) {
            VectorSet(vr.weaponangles, -degrees(atanf(vec[1] / zxDist)),
                      -degrees(atan2f(vec[0], -vec[2])), vr.weaponangles[ROLL] / 2.0f); //Dampen roll on stabilised weapon
        }
    }

}

static void IN_VRJoystick( qboolean isRightController, float joystickX, float joystickY )
{
    char action[256];
	vrController_t* controller = isRightController == qtrue ? &rightController : &leftController;

	if (vr.virtual_screen ||
            cl.snap.ps.pm_type == PM_INTERMISSION)
	{
		const float x = joystickX * 5.0;
		const float y = joystickY * -5.0;

		Com_QueueEvent(in_vrEventTime, SE_MOUSE, x, y, 0, NULL);
	} else
	{
		if (isRightController == qfalse) {
			vec3_t positional;
			VectorClear(positional);

			vec2_t joystick;
            if ( !com_sv_running || !com_sv_running->integer )
            {
                //multiplayer server
                if (!vr_directionMode->integer) {
					//HMD Based
					rotateAboutOrigin(joystickX, joystickY, vr.hmdorientation[YAW], joystick);
				} else {
                	//Off-hand based
					rotateAboutOrigin(joystickX, joystickY, vr.offhandangles[YAW], joystick);
				}
            }
            else
            {
				//Positional movement speed correction for when we are not hitting target framerate
				int refresh = vrapi_GetSystemPropertyInt(&(VR_GetEngine()->java), VRAPI_SYS_PROP_DISPLAY_REFRESH_RATE);
				float multiplier = (float)((1000.0 / refresh) / (in_vrEventTime - lastframetime));

				float factor = (refresh / 72.0F) * 10.0f; // adjust positional factor based on refresh rate
				rotateAboutOrigin(-vr.hmdposition_delta[0] * factor * multiplier,
								  vr.hmdposition_delta[2] * factor * multiplier, -vr.hmdorientation[YAW], positional);

				if (!vr_directionMode->integer) {
					//HMD Based
					joystick[0] = joystickX;
					joystick[1] = joystickY;
				} else {
					//Off-hand based
					rotateAboutOrigin(joystickX, joystickY, vr.offhandangles[YAW] - vr.hmdorientation[YAW], joystick);
				}
            }

            //sideways
            Com_QueueEvent(in_vrEventTime, SE_JOYSTICK_AXIS, 0, joystick[0] * 127.0f + positional[0] * 127.0f, 0, NULL);

            //forward/back
            Com_QueueEvent(in_vrEventTime, SE_JOYSTICK_AXIS, 1, joystick[1] * 127.0f + positional[1] * 127.0f, 0, NULL);
        }
        else //right controller
        {
            //yaw
            if (vr_snapturn->integer > 0)
			{
            	int snap = 45;
            	if (vr_snapturn->integer > 1)
				{
            		snap = vr_snapturn->integer;
				}

				if (!(controller->axisButtons & VR_TOUCH_AXIS_RIGHT) && joystickX > pressedThreshold) {
					controller->axisButtons |= VR_TOUCH_AXIS_RIGHT;
					CL_SnapTurn(snap);
				} else if ((controller->axisButtons & VR_TOUCH_AXIS_RIGHT) &&
						joystickX < releasedThreshold) {
					controller->axisButtons &= ~VR_TOUCH_AXIS_RIGHT;
				}

				if (!(controller->axisButtons & VR_TOUCH_AXIS_LEFT) && joystickX < -pressedThreshold) {
					controller->axisButtons |= VR_TOUCH_AXIS_LEFT;
					CL_SnapTurn(-snap);
				} else if ((controller->axisButtons & VR_TOUCH_AXIS_LEFT) &&
						   joystickX > -releasedThreshold) {
					controller->axisButtons &= ~VR_TOUCH_AXIS_LEFT;
				}
			}
            else
			{
            	//smooth turn
				const float x = joystickX * cl_sensitivity->value * m_yaw->value;
				Com_QueueEvent(in_vrEventTime, SE_MOUSE, x, 0, 0, NULL);
			}

			//Default up/down on right thumbstick is weapon switch
            if (!(controller->axisButtons & VR_TOUCH_AXIS_UP) && joystickY > pressedThreshold)
            {
                controller->axisButtons |= VR_TOUCH_AXIS_UP;
                if (IN_GetButtonAction("RTHUMBFORWARD", action))
                {
                    IN_SendButtonAction(action, qtrue);
                }
            }
            else if ((controller->axisButtons & VR_TOUCH_AXIS_UP) &&
                     joystickY < releasedThreshold)
            {
                if (IN_GetButtonAction("RTHUMBFORWARD", action))
                {
                    IN_SendButtonAction(action, qfalse);
                }
                controller->axisButtons &= ~VR_TOUCH_AXIS_UP;
            }


            if (!(controller->axisButtons & VR_TOUCH_AXIS_DOWN) && joystickY < -pressedThreshold) {
                controller->axisButtons |= VR_TOUCH_AXIS_DOWN;
                if (IN_GetButtonAction("RTHUMBBACK", action))
                {
                    IN_SendButtonAction(action, qtrue);
                }
            } else if ((controller->axisButtons & VR_TOUCH_AXIS_DOWN) &&
                       joystickY > -releasedThreshold) {
                controller->axisButtons &= ~VR_TOUCH_AXIS_DOWN;
                if (IN_GetButtonAction("RTHUMBBACK", action))
                {
                    IN_SendButtonAction(action, qfalse);
                }
            }
        }
    }
}

static void IN_VRTriggers( qboolean isRightController, float index ) {
	vrController_t* controller = isRightController == qtrue ? &rightController : &leftController;

	//Primary controller trigger is mouse click in screen mode or in intermission
	if (VR_useScreenLayer() || cl.snap.ps.pm_type == PM_INTERMISSION)
    {
        if (isRightController == (vr_righthanded->integer != 0))
        {
            if (!(controller->axisButtons & VR_TOUCH_AXIS_TRIGGER_INDEX) &&
                index > pressedThreshold)
            {
                controller->axisButtons |= VR_TOUCH_AXIS_TRIGGER_INDEX;
                Com_QueueEvent(in_vrEventTime, SE_KEY, K_MOUSE1, qtrue, 0, NULL);
            }
            else if ((controller->axisButtons & VR_TOUCH_AXIS_TRIGGER_INDEX) &&
                     index < releasedThreshold)
            {
                controller->axisButtons &= ~VR_TOUCH_AXIS_TRIGGER_INDEX;
                Com_QueueEvent(in_vrEventTime, SE_KEY, K_MOUSE1, qfalse, 0, NULL);
            }
        }
    }
    else
    {
        char action[256];
        
        //Primary trigger
        if (isRightController == (vr_righthanded->integer != 0))
        {
            if (!(controller->axisButtons & VR_TOUCH_AXIS_TRIGGER_INDEX) &&
                index > pressedThreshold)
            {
                controller->axisButtons |= VR_TOUCH_AXIS_TRIGGER_INDEX;
                if (IN_GetButtonAction("PRIMARYTRIGGER", action))
                {
                    IN_SendButtonAction(action, qtrue);
                }
            }
            else if ((controller->axisButtons & VR_TOUCH_AXIS_TRIGGER_INDEX) &&
                     index < releasedThreshold)
            {
                controller->axisButtons &= ~VR_TOUCH_AXIS_TRIGGER_INDEX;
                if (IN_GetButtonAction("PRIMARYTRIGGER", action))
                {
                    IN_SendButtonAction(action, qfalse);
                }
            }
        }

        //off hand trigger
        if (isRightController != (vr_righthanded->integer != 0))
        {
            if (!(controller->axisButtons & VR_TOUCH_AXIS_TRIGGER_INDEX) &&
                index > pressedThreshold)
            {
                controller->axisButtons |= VR_TOUCH_AXIS_TRIGGER_INDEX;
                if (IN_GetButtonAction("SECONDARYTRIGGER", action))
                {
                    IN_SendButtonAction(action, qtrue);
                }
            }
            else if ((controller->axisButtons & VR_TOUCH_AXIS_TRIGGER_INDEX) &&
                     index < releasedThreshold)
            {
                controller->axisButtons &= ~VR_TOUCH_AXIS_TRIGGER_INDEX;
                if (IN_GetButtonAction("SECONDARYTRIGGER", action))
                {
                    IN_SendButtonAction(action, qfalse);
                }
            }
        }
    }
}

static void IN_VRButtonsChanged( qboolean isRightController, uint32_t buttons )
{
    char action[256];

	vrController_t* controller = isRightController == qtrue ? &rightController : &leftController;

	{
        if ((buttons & ovrButton_Enter) && !(controller->buttons & ovrButton_Enter)) {
            Com_QueueEvent(in_vrEventTime, SE_KEY, K_ESCAPE, qtrue, 0, NULL);
        } else if (!(buttons & ovrButton_Enter) && (controller->buttons & ovrButton_Enter)) {
            Com_QueueEvent(in_vrEventTime, SE_KEY, K_ESCAPE, qfalse, 0, NULL);
        }
    }

	if (isRightController == !vr_righthanded->integer)
    {
        if ((buttons & ovrButton_GripTrigger) && !(controller->buttons & ovrButton_GripTrigger))
        {
            if (IN_GetButtonAction("SECONDARYGRIP", action))
            {
                IN_SendButtonAction(action, qtrue);
            }
        }
        else if (!(buttons & ovrButton_GripTrigger) &&
                 (controller->buttons & ovrButton_GripTrigger))
        {
            if (IN_GetButtonAction("SECONDARYGRIP", action))
            {
                IN_SendButtonAction(action, qfalse);
            }
        }
	}
    else
    {
        if ((buttons & ovrButton_GripTrigger) && !(controller->buttons & ovrButton_GripTrigger))
        {
            if (IN_GetButtonAction("PRIMARYGRIP", action))
            {
                IN_SendButtonAction(action, qtrue);
            }
        }
        else if (!(buttons & ovrButton_GripTrigger) &&
                 (controller->buttons & ovrButton_GripTrigger))
        {
            if (IN_GetButtonAction("PRIMARYGRIP", action))
            {
                IN_SendButtonAction(action, qfalse);
            }
        }
    }


    if (isRightController == !vr_righthanded->integer)
    {
        if ((buttons & ovrButton_RThumb) && !(controller->buttons & ovrButton_RThumb)) {
            if (IN_GetButtonAction("SECONDARYTHUMBSTICK", action))
            {
                IN_SendButtonAction(action, qtrue);
            }

            vr.realign = 4;
        } else if (!(buttons & ovrButton_RThumb) && (controller->buttons & ovrButton_RThumb)) {
            if (IN_GetButtonAction("SECONDARYTHUMBSTICK", action))
            {
                IN_SendButtonAction(action, qfalse);
            }
        }
    }
    else
    {
        if ((buttons & ovrButton_RThumb) && !(controller->buttons & ovrButton_RThumb)) {
            if (IN_GetButtonAction("PRIMARYTHUMBSTICK", action))
            {
                IN_SendButtonAction(action, qtrue);
            }
        } else if (!(buttons & ovrButton_RThumb) && (controller->buttons & ovrButton_RThumb)) {
            if (IN_GetButtonAction("PRIMARYTHUMBSTICK", action))
            {
                IN_SendButtonAction(action, qfalse);
            }
        }
    }

    //Jump
    if ((buttons & ovrButton_A) && !(controller->buttons & ovrButton_A))
    {
        if (IN_GetButtonAction("A", action))
        {
            IN_SendButtonAction(action, qtrue);
        }
    }
    else if (!(buttons & ovrButton_A) && (controller->buttons & ovrButton_A))
    {
        if (IN_GetButtonAction("A", action))
        {
            IN_SendButtonAction(action, qfalse);
        }
    }

    //Crouch
    if ((buttons & ovrButton_B) && !(controller->buttons & ovrButton_B)) {
        if (IN_GetButtonAction("B", action))
        {
            IN_SendButtonAction(action, qtrue);
        }
    } else if (!(buttons & ovrButton_B) && (controller->buttons & ovrButton_B)) {
        if (IN_GetButtonAction("B", action))
        {
            IN_SendButtonAction(action, qfalse);
        }
    }

    //X default is "use item"
	if ((buttons & ovrButton_X) && !(controller->buttons & ovrButton_X)) {
        if (IN_GetButtonAction("X", action))
        {
            IN_SendButtonAction(action, qtrue);
        }
	} else if (!(buttons & ovrButton_X) && (controller->buttons & ovrButton_X)) {
        if (IN_GetButtonAction("X", action))
        {
            IN_SendButtonAction(action, qfalse);
        }
	}

    //Y default is Gesture
    if ((buttons & ovrButton_Y) && !(controller->buttons & ovrButton_Y)) {
        if (IN_GetButtonAction("Y", action))
        {
            IN_SendButtonAction(action, qtrue);
        }
    } else if (!(buttons & ovrButton_Y) && (controller->buttons & ovrButton_Y)) {
        if (IN_GetButtonAction("Y", action))
        {
            IN_SendButtonAction(action, qfalse);
        }
    }

	controller->buttons = buttons;
}

void IN_VRInputFrame( void )
{
	if (controllerInit == qfalse) {
		memset(&leftController, 0, sizeof(leftController));
		memset(&rightController, 0, sizeof(rightController));
		controllerInit = qtrue;
	}

	ovrMobile* ovr = VR_GetEngine()->ovr;
	if (!ovr) {
		return;
	}

    ovrResult result;
	if (vr_extralatencymode != NULL &&
            vr_extralatencymode->integer) {
        result = vrapi_SetExtraLatencyMode(VR_GetEngine()->ovr, VRAPI_EXTRA_LATENCY_MODE_ON);
        assert(result == VRAPI_INITIALIZE_SUCCESS);
    }

	if (vr_refreshrate != NULL && vr_refreshrate->integer)
	{
		vrapi_SetDisplayRefreshRate(VR_GetEngine()->ovr, (float)vr_refreshrate->integer);
	}

    result = vrapi_SetClockLevels(VR_GetEngine()->ovr, 4, 4);
    assert(result == VRAPI_INITIALIZE_SUCCESS);

	vr.virtual_screen = VR_useScreenLayer();

	//trigger frame tick for haptics
    VR_HapticEvent("frame_tick", 0, 0, 0, 0, 0);

    {
		// We extract Yaw, Pitch, Roll instead of directly using the orientation
		// to allow "additional" yaw manipulation with mouse/controller.
		const ovrQuatf quatHmd =  VR_GetEngine()->tracking.HeadPose.Pose.Orientation;
		const ovrVector3f positionHmd = VR_GetEngine()->tracking.HeadPose.Pose.Position;
		vec3_t rotation = {0, 0, 0};
		QuatToYawPitchRoll(quatHmd, rotation, vr.hmdorientation);
		VectorSet(vr.hmdposition, positionHmd.x, positionHmd.y + vr_heightAdjust->value, positionHmd.z);

		//Position
		VectorSubtract(vr.hmdposition_last, vr.hmdposition, vr.hmdposition_delta);

		//Keep this for our records
		VectorCopy(vr.hmdposition, vr.hmdposition_last);

		//Orientation
		VectorSubtract(vr.hmdorientation_last, vr.hmdorientation, vr.hmdorientation_delta);

		//Keep this for our records
		VectorCopy(vr.hmdorientation, vr.hmdorientation_last);
	}


	ovrInputCapabilityHeader capsHeader;
	uint32_t index = 0;
	for (;;) {
		ovrResult enumResult = vrapi_EnumerateInputDevices(ovr, index, &capsHeader);
		if (enumResult < 0) {
			break;
		}
		++index;
		
		if (capsHeader.Type != ovrControllerType_TrackedRemote) {
			continue;
		}

		ovrInputTrackedRemoteCapabilities caps;
		caps.Header = capsHeader;
		ovrResult capsResult = vrapi_GetInputDeviceCapabilities(ovr, &caps.Header);
		if (capsResult < 0) {
			continue;
		}

		ovrInputStateTrackedRemote state;
		state.Header.ControllerType = ovrControllerType_TrackedRemote;
		ovrResult stateResult = vrapi_GetCurrentInputState(ovr, capsHeader.DeviceID, &state.Header);
		if (stateResult < 0) {
			continue;
		}

		ovrTracking remoteTracking;
		stateResult = vrapi_GetInputTrackingState(ovr, capsHeader.DeviceID, VR_GetEngine()->predictedDisplayTime,
											 &remoteTracking);
		if (stateResult < 0) {
			continue;
		}


		qboolean isRight;
		vrController_t* controller;
		if (caps.ControllerCapabilities & ovrControllerCaps_LeftHand) {
			isRight = qfalse;
			controller = &leftController;
		} else if (caps.ControllerCapabilities & ovrControllerCaps_RightHand) {
			isRight = qtrue;
			controller = &rightController;
		}
		else {
			continue;
		}

        if (controller->buttons ^ state.Buttons) {
            IN_VRButtonsChanged(isRight, state.Buttons);
        }

		IN_VRController(isRight, remoteTracking);
		IN_VRJoystick(isRight, state.Joystick.x, state.Joystick.y);
		IN_VRTriggers(isRight, state.IndexTrigger);
	}

	lastframetime = in_vrEventTime;
	in_vrEventTime = Sys_Milliseconds( );
}

//#endif