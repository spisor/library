#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <stdlib.h>
#include <string.h>

#include "secret_backend.h"

#define SERVICE_NAME "spisor"

static runtime_secret_answer answer_for(OSStatus status) {
    switch (status) {
    case errSecSuccess:
        return RUNTIME_SECRET_DONE;
    case errSecItemNotFound:
        return RUNTIME_SECRET_ABSENT;
    case errSecUserCanceled:
    case errSecAuthFailed:
        return RUNTIME_SECRET_DECLINED;
    case errSecNotAvailable:
    case errSecInteractionNotAllowed:
    case errSecNoDefaultKeychain:
        return RUNTIME_SECRET_UNAVAILABLE;
    default:
        return RUNTIME_SECRET_FAILED;
    }
}

static CFStringRef make_string(const char *text) {
    return CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);
}

static CFMutableDictionaryRef make_query(const char *account) {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = make_string(SERVICE_NAME);
    CFStringRef account_text = account != NULL ? make_string(account) : NULL;
    if (query == NULL || service == NULL || (account != NULL && account_text == NULL)) {
        if (query != NULL) {
            CFRelease(query);
        }
        query = NULL;
    } else {
        CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query, kSecAttrService, service);
        if (account_text != NULL) {
            CFDictionarySetValue(query, kSecAttrAccount, account_text);
        }
    }
    if (service != NULL) {
        CFRelease(service);
    }
    if (account_text != NULL) {
        CFRelease(account_text);
    }
    return query;
}

static CFDataRef make_data(const char *value) {
    return CFDataCreate(kCFAllocatorDefault, (const UInt8 *)value, (CFIndex)strlen(value));
}

static OSStatus add_item(CFDictionaryRef query, CFDataRef data) {
    CFMutableDictionaryRef item = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, query);
    OSStatus status = errSecAllocate;
    if (item != NULL) {
        CFDictionarySetValue(item, kSecValueData, data);
        status = SecItemAdd(item, NULL);
        CFRelease(item);
    }
    return status;
}

static runtime_secret_answer update_item(CFDictionaryRef query, CFDataRef data) {
    CFMutableDictionaryRef changes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks);
    OSStatus status = errSecAllocate;
    if (changes != NULL) {
        CFDictionarySetValue(changes, kSecValueData, data);
        status = SecItemUpdate(query, changes);
        CFRelease(changes);
    }
    return answer_for(status);
}

static char *copy_utf8(CFStringRef text) {
    CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    char *copy = malloc((size_t)size);
    if (copy != NULL && !CFStringGetCString(text, copy, size, kCFStringEncodingUTF8)) {
        free(copy);
        copy = NULL;
    }
    return copy;
}

static char *copy_data_as_text(CFDataRef data) {
    CFIndex length = CFDataGetLength(data);
    char *text = malloc((size_t)length + 1);
    if (text != NULL) {
        memcpy(text, CFDataGetBytePtr(data), (size_t)length);
        text[length] = '\0';
    }
    return text;
}

static runtime_secret_answer collect_accounts(CFArrayRef items, const char *prefix, runtime_secret_names *names) {
    CFIndex count = CFArrayGetCount(items);
    names->names = calloc(count > 0 ? (size_t)count : 1, sizeof(char *));
    if (names->names == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    for (CFIndex index = 0; index < count; index++) {
        CFDictionaryRef item = CFArrayGetValueAtIndex(items, index);
        CFStringRef account = CFDictionaryGetValue(item, kSecAttrAccount);
        char *name = account != NULL ? copy_utf8(account) : NULL;
        if (account != NULL && name == NULL) {
            runtime_free_secret_names(names);
            return RUNTIME_SECRET_FAILED;
        }
        if (name != NULL && strncmp(name, prefix, strlen(prefix)) == 0) {
            names->names[names->count++] = name;
        } else {
            free(name);
        }
    }
    return RUNTIME_SECRET_DONE;
}

int runtime_secret_store_available(void) {
    CFMutableDictionaryRef query = make_query("availability-probe");
    OSStatus status = errSecAllocate;
    if (query != NULL) {
        status = SecItemCopyMatching(query, NULL);
        CFRelease(query);
    }
    return status == errSecSuccess || status == errSecItemNotFound;
}

runtime_secret_answer runtime_secret_store_get(const char *entry, char **value) {
    CFMutableDictionaryRef query = make_query(entry);
    CFTypeRef found = NULL;
    OSStatus status = errSecAllocate;
    *value = NULL;
    if (query != NULL) {
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
        status = SecItemCopyMatching(query, &found);
        CFRelease(query);
    }
    if (status == errSecSuccess) {
        if (found == NULL) {
            return RUNTIME_SECRET_FAILED;
        }
        *value = copy_data_as_text((CFDataRef)found);
        CFRelease(found);
        return *value != NULL ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_FAILED;
    }
    return answer_for(status);
}

runtime_secret_answer runtime_secret_store_set(const char *entry, const char *value) {
    CFMutableDictionaryRef query = make_query(entry);
    CFDataRef data = make_data(value);
    runtime_secret_answer answer = RUNTIME_SECRET_FAILED;
    if (query != NULL && data != NULL) {
        OSStatus added = add_item(query, data);
        answer = added == errSecDuplicateItem ? update_item(query, data) : answer_for(added);
    }
    if (query != NULL) {
        CFRelease(query);
    }
    if (data != NULL) {
        CFRelease(data);
    }
    return answer;
}

runtime_secret_answer runtime_secret_store_delete(const char *entry) {
    CFMutableDictionaryRef query = make_query(entry);
    OSStatus status = errSecAllocate;
    if (query != NULL) {
        status = SecItemDelete(query);
        CFRelease(query);
    }
    return answer_for(status);
}

runtime_secret_answer runtime_secret_store_list(const char *prefix, runtime_secret_names *names) {
    CFMutableDictionaryRef query = make_query(NULL);
    CFTypeRef found = NULL;
    OSStatus status = errSecAllocate;
    names->names = NULL;
    names->count = 0;
    if (query != NULL) {
        CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitAll);
        status = SecItemCopyMatching(query, &found);
        CFRelease(query);
    }
    if (status == errSecItemNotFound) {
        return RUNTIME_SECRET_DONE;
    }
    if (status != errSecSuccess || found == NULL) {
        return answer_for(status);
    }
    runtime_secret_answer answer = collect_accounts((CFArrayRef)found, prefix, names);
    CFRelease(found);
    return answer;
}
