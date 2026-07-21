#include "cmny_paths.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char path[256] = {0};
    char err[128] = {0};

    ASSERT_TRUE(setenv("CMNY_DB", "/tmp/cmny-environment.db", 1) == 0);
    ASSERT_TRUE(setenv("CMNY_HOME", "/tmp/cmny-environment-home", 1) == 0);

    ASSERT_TRUE(cmny_resolve_db_path_for_home("/tmp/cmny-explicit.db", NULL,
                                              path, sizeof(path), err, sizeof(err)));
    ASSERT_TRUE(strcmp(path, "/tmp/cmny-explicit.db") == 0);

    ASSERT_TRUE(cmny_resolve_db_path_for_home(NULL, "/tmp/cmny-portable/",
                                              path, sizeof(path), err, sizeof(err)));
    ASSERT_TRUE(strcmp(path, "/tmp/cmny-portable/cmny.db") == 0);

    ASSERT_TRUE(cmny_resolve_db_path(NULL, path, sizeof(path), err, sizeof(err)));
    ASSERT_TRUE(strcmp(path, "/tmp/cmny-environment.db") == 0);

    ASSERT_TRUE(unsetenv("CMNY_DB") == 0);
    ASSERT_TRUE(cmny_resolve_db_path_for_home(NULL, NULL, path, sizeof(path), err, sizeof(err)));
    ASSERT_TRUE(strcmp(path, "/tmp/cmny-environment-home/cmny.db") == 0);

    char tiny[8] = {0};
    ASSERT_TRUE(!cmny_resolve_db_path_for_home(NULL, "/tmp/cmny-portable",
                                               tiny, sizeof(tiny), err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "too long") != NULL);

    ASSERT_TRUE(unsetenv("CMNY_HOME") == 0);
    (void)printf("ok - path tests\n");
    return 0;
}
