#include <EndpointSecurity/EndpointSecurity.h>
#include <dispatch/dispatch.h>
#include <bsm/libbsm.h>
#include <stdio.h>
#include <os/log.h>
#include <Security/Authorization.h>

static dispatch_queue_t g_event_queue = NULL;

static void
init_dispatch_queue(void)
{
    // Choose an appropriate Quality of Service class appropriate for your app.
    // https://developer.apple.com/documentation/dispatch/dispatchqos
    dispatch_queue_attr_t queue_attrs = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0);

    g_event_queue = dispatch_queue_create("event_queue", queue_attrs);
}

static void
authorize()
{
    OSStatus stat;
    AuthorizationItem taskport_item[] = {{"system.privilege.taskport:"}};
    AuthorizationRights rights = {1, taskport_item}, *out_rights = NULL;
    AuthorizationRef author;
    
    AuthorizationFlags auth_flags = kAuthorizationFlagExtendRights | kAuthorizationFlagPreAuthorize | kAuthorizationFlagInteractionAllowed | ( 1 << 5);
    
    stat = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,auth_flags,&author);
    if (stat != errAuthorizationSuccess) {
        os_log_error(OS_LOG_DEFAULT, "AuthorizationCreate failed with %{public}d", stat);
    }
    
    stat = AuthorizationCopyRights(author, &rights, kAuthorizationEmptyEnvironment, auth_flags,&out_rights);
    if (stat != errAuthorizationSuccess) {
        os_log_error(OS_LOG_DEFAULT, "AuthorizationCopyRights failed with %{public}d", stat);
    }
}

static bool
is_eicar_file(const es_file_t *file)
{
    // The EICAR test file string, as defined by the EICAR standard.
    static const char* eicar = "X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";
    static const off_t eicar_length = sizeof(eicar) - 1;
    static const off_t eicar_max_length = 128;

    bool result = false;

    // EICAR check
    // First: ensure the length matches defined EICAR requirements.
    if (file->stat.st_size >= eicar_length && file->stat.st_size <= eicar_max_length) {
        //Second: Open the file and read the data.
        int fd = open(file->path.data, O_RDONLY);
        if (fd >= 0) {
            uint8_t buf[sizeof(eicar)];
            ssize_t bytes_read = read(fd, buf, sizeof(buf));
            if (bytes_read >= eicar_length) {
                //Third: Test the file contents against the EICAR test string.
                if (memcmp(buf, eicar, sizeof(buf)) == 0) {
                    result = true;
                }
            }

            close(fd);
        }
    }

    return result;
}

static void
handle_exec(es_client_t *client, const es_message_t *msg)
{
    // To keep the code simple, this example denies execution based on signing ID.
    // However this isn't a very restrictive policy and could inadvertently lead to
    // denying more executions than intended. In general, you should consider using
    // more restrictive policies like inspecting the process's CDHash instead.
    kern_return_t status = KERN_FAILURE;
    static const char signing_id_to_block[] = "com.apple.TextEdit";
    es_string_token_t *current_signing_id = &msg->event.exec.target->signing_id;
    
    pid_t pid = audit_token_to_pid(msg->event.exec.target->audit_token);
    
    mach_port_t task;
    status = task_for_pid(mach_task_self(), pid, &task);
    if (status != KERN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "task_for_pid failed with %{public}d", status);
    } else {
        os_log_info(OS_LOG_DEFAULT, "task_for_pid success: %d", task);
    }
    
    if ((current_signing_id->length == sizeof(signing_id_to_block) - 1) &&
            (strncmp(current_signing_id->data, signing_id_to_block, sizeof(signing_id_to_block)) == 0)) {
        es_respond_auth_result(client, msg, ES_AUTH_RESULT_DENY, true);
    } else {
        es_respond_auth_result(client, msg, ES_AUTH_RESULT_ALLOW, true);
    }
}

static void
handle_open_worker(es_client_t *client, const es_message_t *msg)
{
    static const char *ro_prefix = "/usr/local/bin/";
    static const size_t ro_prefix_length = sizeof(ro_prefix) - 1;

    if (is_eicar_file(msg->event.open.file)) {
        // Don't allow any operations on EICAR files.
        es_respond_flags_result(client, msg, 0, true);
    } else if (strncmp(msg->event.open.file->path.data, ro_prefix, ro_prefix_length) == 0) {
        // Deny writing to paths that match the readonly prefix.
        es_respond_flags_result(client, msg, 0xffffffff & ~FWRITE, true);
    } else {
        // Allow everything else...
        es_respond_flags_result(client, msg, 0xffffffff, true);
    }
}

static void
handle_open(es_client_t *client, const es_message_t *msg)
{
    // Note: `es_retain_message` and `es_release_message` are only available in
    // macOS 11.0 and newer. To run this sample project on macOS 10.15, first
    // update the deployment target in the project settings, then modify this
    // function to use the older `es_copy_message` and `es_free_message` APIs.
    es_retain_message(msg);

    dispatch_async(g_event_queue, ^{
        handle_open_worker(client, msg);
        es_release_message(msg);
    });
}

static void
handle_notify_exec(es_client_t *client, const es_message_t *msg)
{
    os_log(OS_LOG_DEFAULT, "%{public}s (pid: %d) | EXEC: New image: %{public}s",
        msg->process->executable->path.data,
        audit_token_to_pid(msg->process->audit_token),
        msg->event.exec.target->executable->path.data);
}

static void
handle_notify_fork(es_client_t *client, const es_message_t *msg)
{
    os_log(OS_LOG_DEFAULT, "%{public}s (pid: %d) | FORK: Child pid: %d",
        msg->process->executable->path.data,
        audit_token_to_pid(msg->process->audit_token),
        audit_token_to_pid(msg->event.fork.child->audit_token));
}

static void
handle_notify_exit(es_client_t *client, const es_message_t *msg)
{
    os_log(OS_LOG_DEFAULT, "%{public}s (pid: %d) | EXIT: status: %d",
        msg->process->executable->path.data,
        audit_token_to_pid(msg->process->audit_token),
        msg->event.exit.stat);
}

static void
handle_event(es_client_t *client, const es_message_t *msg)
{

    switch (msg->event_type) {
        case ES_EVENT_TYPE_AUTH_EXEC:
            handle_exec(client, msg);
            break;

        case ES_EVENT_TYPE_AUTH_OPEN:
            handle_open(client, msg);
            break;
            
        case ES_EVENT_TYPE_NOTIFY_EXEC:
            handle_notify_exec(client, msg);
            break;
            
        case ES_EVENT_TYPE_NOTIFY_FORK:
            handle_notify_fork(client, msg);
            break;
            
        case ES_EVENT_TYPE_NOTIFY_EXIT:
            handle_notify_exit(client, msg);
            break;

        default:
            if (msg->action_type == ES_ACTION_TYPE_AUTH) {
                es_respond_auth_result(client, msg, ES_AUTH_RESULT_ALLOW, true);
            }
            break;
    }
}

int
main(int argc, char *argv[])
{
    authorize();
    
    init_dispatch_queue();

    es_client_t *client;
    es_new_client_result_t result = es_new_client(&client, ^(es_client_t *c, const es_message_t *msg) {
        handle_event(c, msg);
    });

    if (result != ES_NEW_CLIENT_RESULT_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "Failed to create the ES client: %d", result);
        return 1;
    }

    es_event_type_t events[] = {
        ES_EVENT_TYPE_AUTH_EXEC,
        ES_EVENT_TYPE_AUTH_OPEN,
        ES_EVENT_TYPE_NOTIFY_EXEC,
        ES_EVENT_TYPE_NOTIFY_FORK,
        ES_EVENT_TYPE_NOTIFY_EXIT
    };
    if (es_subscribe(client, events, sizeof(events) / sizeof(events[0])) != ES_RETURN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "Failed to subscribe to events");
        es_delete_client(client);
        return 1;
    }

    dispatch_main();

    return 0;
}
