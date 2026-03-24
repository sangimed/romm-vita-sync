#ifndef PS1_PATHS_H
#define PS1_PATHS_H

static const char *kPs1VmpCandidateRoots[] = {
    "ux0:pspemu/PSP/SAVEDATA"
};

#define PS1_VMP_CANDIDATE_ROOT_COUNT (sizeof(kPs1VmpCandidateRoots) / sizeof(kPs1VmpCandidateRoots[0]))

#endif
